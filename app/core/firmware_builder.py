"""
Compila il firmware direttamente sul server (PlatformIO). Tre target possibili:

- "main": il firmware definitivo (display/touch/LVGL/widget), con host/porta/
  device_id scelti dalla web UI iniettati in device_config.h.
- "loader": build minima (firmware/loader/, progetto PlatformIO separato,
  nessun display/LVGL/BSP) usata solo per scansionare le reti WiFi e testare
  una connessione via USB dalla web UI (vedi PROTOCOL.md). Molto più veloce
  da compilare del firmware definitivo.
- "loader_full": stesso scopo del loader, ma con schermo/touch attivi
  (firmware/loader-full/, riusa la BSP e lv_conf.h del firmware definitivo):
  lista reti/password/connessione anche direttamente sul display, più una
  schermata Impostazioni con MAC WiFi/Bluetooth e scansione/connessione BLE.
  Stesso protocollo seriale del loader leggero: il pannello "Test connessione
  WiFi" della web UI funziona invariato con entrambi.

In tutti i casi il risultato finale è un unico .bin (bootloader+partizioni+
app uniti con `esptool merge_bin`) depositato nel magazzino firmware
(firmware_store), pronto per il flashing da browser via Web Serial. I file
dei due loader sono salvati con suffisso "_loader"/"_loaderfull" per
distinguerli nell'elenco.

Le credenziali WiFi non passano MAI da qui, nemmeno per i loader: sono
pilotate in tempo reale (via Web Serial o touchscreen) mentre il device è
collegato in USB, non da parametri di build.

Gira in un thread di background: la build PlatformIO può richiedere svariate
decine di secondi (soprattutto la prima volta, quando scarica toolchain e
librerie), quindi non deve bloccare la richiesta HTTP che la avvia.
"""
import logging
import os
import re
import shutil
import subprocess
import threading
import time
import uuid
from typing import Any, Dict, Optional, Tuple

from app.core import firmware_store

logger = logging.getLogger(__name__)

_FIRMWARE_ROOT = os.path.join(os.path.dirname(__file__), "..", "..", "firmware")
_LOADER_ROOT = os.path.join(_FIRMWARE_ROOT, "loader")
_LOADER_FULL_ROOT = os.path.join(_FIRMWARE_ROOT, "loader-full")
# Gateway Bluetooth: ESP32 CLASSICO (Bluetooth Classic per l'A2DP verso le casse)
_GATEWAY_ROOT = os.path.join(_FIRMWARE_ROOT, "gateway")

_TEMPLATE_PATH = os.path.join(_FIRMWARE_ROOT, "include", "device_config.h.template")
_CONFIG_OUTPUT_PATH = os.path.join(_FIRMWARE_ROOT, "include", "device_config.h")

_MAIN_BUILD_ENV = "esp32-s3-devkitc-1"
_LOADER_BUILD_ENV = "wifi-setup-loader"
_LOADER_FULL_BUILD_ENV = "esp32-s3-devkitc-1-loader-full"
_GATEWAY_BUILD_ENV = "esp-wrover-kit"

# Chip e indirizzo del bootloader per il .bin unico (merge_bin): sull'ESP32-S3 il
# bootloader sta a 0x0, sull'ESP32 classico a 0x1000.
_CHIP = {"gateway": ("esp32", "0x1000")}
_DEFAULT_CHIP = ("esp32s3", "0x0")

_VERSION_FILE = os.path.join(_FIRMWARE_ROOT, "VERSION")

_lock = threading.Lock()
_current_job: Optional[Dict[str, Any]] = None


def read_tracked_version() -> str:
    """Versione di default proposta nella web UI: il file firmware/VERSION viene
    aggiornato ogni volta che il codice del firmware cambia in modo sostanziale,
    così il campo "Versione" nella sezione di generazione parte già valorizzato
    invece che con un placeholder statico."""
    try:
        with open(_VERSION_FILE, "r") as f:
            value = f.read().strip()
            return value or "1.0.0"
    except OSError:
        return "1.0.0"


def _sanitize_filename_part(value: str) -> str:
    value = (value or "").strip()
    return re.sub(r'[^A-Za-z0-9_.-]+', '_', value) or "x"


def _build_merged_filename(target: str, params: Dict[str, Any]) -> str:
    """Nome del .bin generato (e del suo file di log, stesso nome base): per il
    firmware definitivo codifica modalità-data-versione-revisione, così i file
    restano riconoscibili a colpo d'occhio nell'elenco senza doverli aprire."""
    if target == "loader":
        return f"{int(time.time())}_loader.bin"
    if target == "loader_full":
        return f"{int(time.time())}_loaderfull{'-sd' if params.get('config_sd') else ''}.bin"
    if target == "gateway":
        # con un nome scelto: <timestamp>_gateway-<id>.bin (utile con piu' gateway nell'elenco)
        name = (params.get("gateway_name") or "").strip()
        return f"{int(time.time())}_gateway-{gateway_identity(name)[1]}.bin" if name else f"{int(time.time())}_gateway.bin"

    # usb / wifi / wifi_vpn (+ "-sd" con la configurazione su microSD)
    comm_mode = params.get("comm_mode", "wifi")
    if comm_mode not in COMM_MODES:
        comm_mode = "wifi"
    if comm_mode == "wifi_vpn" and params.get("config_storage_sd"):
        comm_mode += "-sd"
    date_str = time.strftime("%Y%m%d")
    version = _sanitize_filename_part(params.get("fw_version") or read_tracked_version())
    revision = _sanitize_filename_part(params.get("fw_revision") or "") if params.get("fw_revision") else ""
    device_id = _sanitize_filename_part(params.get("device_id") or "display")

    parts = [comm_mode, date_str, version]
    if revision:
        parts.append(revision)
    base_stem = "-".join(parts) + f"_{device_id}"

    # Evita di sovrascrivere un file già esistente con lo stesso nome (stessa
    # combinazione generata più volte lo stesso giorno): aggiunge un contatore
    # solo se serve davvero.
    candidate = f"{base_stem}.bin"
    counter = 2
    while firmware_store.exists(candidate):
        candidate = f"{base_stem}-{counter}.bin"
        counter += 1
    return candidate


# Modalità di build del firmware principale:
#   "usb"      -> USB + Bluetooth
#   "wifi"     -> WiFi diretto + Bluetooth (default, come prima)
#   "wifi_vpn" -> Completo: WiFi + VPN Tailscale + Bluetooth
COMM_MODES = ("usb", "wifi", "wifi_vpn")


def validate_main_params(params: Dict[str, Any]) -> Optional[str]:
    """Ritorna un messaggio d'errore se i parametri non sono validi, altrimenti None."""
    comm_mode = params.get("comm_mode", "wifi")
    if comm_mode not in COMM_MODES:
        return f"Modalità non valida: {comm_mode} (attese: {', '.join(COMM_MODES)})"
    server_host = params.get("server_host", "")
    device_id = params.get("device_id", "")

    # In modalità USB il firmware non fa mai richieste HTTP dirette: è il
    # "ponte" lato browser a parlare col server con la propria origine, quindi
    # server_host/porta incorporati nel firmware non servono a nulla.
    if comm_mode != "usb" and not server_host:
        return "Indirizzo del server mancante"
    if not device_id or not re.match(r'^[a-zA-Z0-9_-]{1,32}$', device_id):
        return "device_id mancante o non valido (lettere, numeri, - e _ , max 32 caratteri)"
    if comm_mode == "wifi_vpn":
        # Facoltativo: se il server LAN (server_host) sta in una subnet pubblicata
        # da un subnet router della tailnet, il firmware lo raggiunge dentro il
        # tunnel con lo stesso indirizzo. Serve solo se il server ha Tailscale
        # installato e non c'è un subnet router (vedi PROTOCOL.md).
        host_vpn = (params.get("server_host_vpn") or "").strip()
        if host_vpn and not re.match(r'^[A-Za-z0-9.-]{1,253}$', host_vpn):
            return "Indirizzo del server via VPN non valido"
        key = (params.get("vpn_auth_key") or "").strip()
        if key and (len(key) < 16 or re.search(r"\s", key)):
            return "Auth key Tailscale non valida (almeno 16 caratteri, senza spazi)"
        hostname = (params.get("vpn_hostname") or "").strip()
        if hostname and not re.match(r'^[a-zA-Z0-9-]{1,63}$', hostname):
            return "Nome sulla tailnet non valido (lettere, numeri e -, max 63)"
        eph = (params.get("vpn_auth_key_ephemeral") or "").strip()
        if eph and (len(eph) < 16 or re.search(r"\s", eph)):
            return "Auth key effimera non valida (almeno 16 caratteri, senza spazi)"
    return None


def _escape_c_string(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def _combined_fw_version(params: Dict[str, Any]) -> str:
    """Versione+revisione unite in una sola stringa (es. "2.0.0-r1"), quella che
    il firmware incorpora davvero e riporta a hello/poll — usata sia per
    generare device_config.h sia per il nome del file, così quello che vedi
    nell'elenco dispositivi corrisponde esattamente a quello nel nome del file."""
    version = (params.get("fw_version") or read_tracked_version()).strip()
    revision = (params.get("fw_revision") or "").strip()
    return f"{version}-{revision}" if revision else version


def _render_main_config(params: Dict[str, Any]):
    with open(_TEMPLATE_PATH, "r") as f:
        template = f.read()

    comm_mode = params.get("comm_mode", "wifi")
    comm_mode_usb = "1" if comm_mode == "usb" else "0"
    vpn = comm_mode == "wifi_vpn"
    replacements = {
        "__COMM_MODE_USB__": comm_mode_usb,
        "__FEATURE_VPN__": "1" if vpn else "0",
        "__VPN_DEFAULT_ENABLED__": "1" if vpn and params.get("vpn_default_enabled", True) else "0",
        # Senza VPN questi restano vuoti anche se il form li contiene.
        "__SERVER_HOST_VPN__": _escape_c_string((params.get("server_host_vpn") or "").strip() if vpn else ""),
        "__VPN_AUTH_KEY__": _escape_c_string((params.get("vpn_auth_key") or "").strip() if vpn else ""),
        "__VPN_HOSTNAME__": _escape_c_string((params.get("vpn_hostname") or "").strip() if vpn else ""),
        "__VPN_AUTH_KEY_EPHEMERAL__": _escape_c_string((params.get("vpn_auth_key_ephemeral") or "").strip() if vpn else ""),
        # Configurazione su microSD: opzione della sola modalità Completo.
        "__CONFIG_STORAGE_SD__": "1" if vpn and params.get("config_storage_sd") else "0",
        "__WIFI_SSID__": _escape_c_string(params.get("wifi_ssid", "")),
        "__WIFI_PASSWORD__": _escape_c_string(params.get("wifi_password", "")),
        "__SERVER_HOST__": _escape_c_string(params.get("server_host", "")),
        "__SERVER_PORT__": str(int(params.get("server_port", 12000))),
        "__DEVICE_ID__": _escape_c_string(params["device_id"]),
        "__FW_VERSION__": _escape_c_string(_combined_fw_version(params)),
        "__POLL_INTERVAL_MS__": str(int(params.get("poll_interval_ms", 4000))),
    }
    for placeholder, value in replacements.items():
        if placeholder in ("__SERVER_PORT__", "__POLL_INTERVAL_MS__", "__COMM_MODE_USB__",
                           "__FEATURE_VPN__", "__VPN_DEFAULT_ENABLED__", "__CONFIG_STORAGE_SD__"):
            template = template.replace(placeholder, value)
        else:
            template = template.replace(f'"{placeholder}"', f'"{value}"')

    with open(_CONFIG_OUTPUT_PATH, "w") as f:
        f.write(template)


def get_status() -> Optional[Dict[str, Any]]:
    with _lock:
        return dict(_current_job) if _current_job else None


def start_build(params: Dict[str, Any]) -> str:
    """Avvia la build del firmware DEFINITIVO."""
    error = validate_main_params(params)
    if error:
        raise ValueError(error)
    return _start_job(target="main", params=dict(params))


def start_loader_build() -> str:
    """Avvia la build del LOADER leggero di test WiFi: nessun parametro richiesto."""
    return _start_job(target="loader", params={})


def start_loader_full_build(config_sd: bool = False) -> str:
    """Avvia la build del LOADER "pesante" (schermo/touch/WiFi/Bluetooth/VPN).
    config_sd: configurazione nel file displayhub.txt sulla microSD (solo RAM
    senza scheda, nessun dato in NVS) invece che nella NVS."""
    return _start_job(target="loader_full", params={"config_sd": bool(config_sd)})


_GATEWAY_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 _-]{0,23}$")


def gateway_identity(name: str) -> Tuple[str, str]:
    """(nome, id sul server) di un gateway dal nome scelto alla compilazione. Il nome e' libero
    (lettere, numeri, spazio, - e _, max 24); l'id ne deriva ("Gateway Salotto" -> "gateway-salotto")
    ed e' quello del file data/BT_setting/<id>.ini. Vuoto = i predefiniti del firmware."""
    name = (name or "").strip()
    if not name:
        return "DH-Gateway", "gateway-01"
    if not _GATEWAY_NAME_RE.match(name):
        raise ValueError("Nome del gateway non valido: da 1 a 24 caratteri tra lettere, numeri, spazio, - e _ "
                         "(deve iniziare con una lettera o un numero)")
    return name, re.sub(r"[ _]+", "-", name).lower()


def _render_gateway_build(params: Dict[str, Any]):
    """Scrive gateway_build.h con nome e id scelti, che il firmware usa come predefiniti."""
    name, gid = gateway_identity(params.get("gateway_name", ""))
    header = os.path.join(_GATEWAY_ROOT, "include", "gateway_build.h")   # al momento: segue _GATEWAY_ROOT
    os.makedirs(os.path.dirname(header), exist_ok=True)
    with open(header, "w", encoding="utf-8") as f:
        f.write("#pragma once\n// GENERATO dal builder prima di ogni build del gateway: non modificare a mano.\n"
                f'#define GW_NAME "{_escape_c_string(name)}"\n#define GW_DEVICE_ID "{_escape_c_string(gid)}"\n')


def start_gateway_build(name: str = "") -> str:
    """Avvia la build del GATEWAY Bluetooth (ESP32 classico). Il nome (facoltativo) e' quello che
    comparira' nelle Impostazioni del display; WiFi e cassa si configurano dopo, via USB o dal
    display. Nome non valido: ValueError."""
    gateway_identity(name)   # validazione subito, prima di avviare il lavoro
    return _start_job(target="gateway", params={"gateway_name": (name or "").strip()})


def _start_job(target: str, params: Dict[str, Any]) -> str:
    global _current_job
    with _lock:
        if _current_job and _current_job.get("state") == "running":
            raise RuntimeError("Una build è già in corso")
        job_id = str(uuid.uuid4())
        _current_job = {
            "job_id": job_id,
            "target": target,
            "state": "running",
            "log": [],
            "started_at": time.time(),
            "finished_at": None,
            "output_filename": None,
        }

    thread = threading.Thread(target=_run_build, args=(job_id, target, params), daemon=True)
    thread.start()
    return job_id


def _append_log(job_id: str, line: str):
    with _lock:
        if _current_job and _current_job["job_id"] == job_id:
            _current_job["log"].append(line)


def _run_subprocess(job_id: str, cmd, cwd=None, extra_env: Optional[Dict[str, str]] = None) -> bool:
    _append_log(job_id, f"$ {' '.join(cmd)}" + (f"   (env: {extra_env})" if extra_env else ""))
    env = None
    if extra_env:
        env = dict(os.environ)
        env.update(extra_env)
    try:
        process = subprocess.Popen(
            cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1, env=env
        )
        for line in process.stdout:
            _append_log(job_id, line.rstrip())
        process.wait()
        return process.returncode == 0
    except Exception as exc:
        _append_log(job_id, f"Errore avviando il comando: {exc}")
        return False


def clear_dependency_cache(target: str) -> str:
    """Cancella .pio/libdeps/<env> (le librerie scaricate) per il target indicato,
    forzando un ridownload completo alla build successiva. Da usare solo quando
    serve davvero (es. dopo aver cambiato una versione pinnata in library.json):
    le build normali NON toccano più questa cache, per non riscaricare tutto ad
    ogni tentativo."""
    with _lock:
        if _current_job and _current_job.get("state") == "running":
            raise RuntimeError("Una build è in corso: attendi che finisca prima di pulire la cache")

    if target == "loader":
        project_dir = _LOADER_ROOT
        build_env = _LOADER_BUILD_ENV
    elif target == "loader_full":
        project_dir = _LOADER_FULL_ROOT
        build_env = _LOADER_FULL_BUILD_ENV
    elif target == "gateway":
        project_dir = _GATEWAY_ROOT
        build_env = _GATEWAY_BUILD_ENV
    elif target == "main":
        project_dir = _FIRMWARE_ROOT
        build_env = _MAIN_BUILD_ENV
    else:
        raise ValueError(f"target sconosciuto: {target}")

    libdeps_dir = os.path.join(project_dir, ".pio", "libdeps", build_env)
    build_dir = os.path.join(project_dir, ".pio", "build", build_env)
    removed = []
    for stale_dir in (libdeps_dir, build_dir):
        if os.path.isdir(stale_dir):
            shutil.rmtree(stale_dir, ignore_errors=True)
            removed.append(stale_dir)
    return f"Cache rimossa: {', '.join(removed) if removed else 'nessuna cache presente'}"


def _patch_lvgl_pragma(project_dir: str, build_env: str, job_id: str):
    """Silenzia il messaggio informativo (non un errore) che LVGL stampa per OGNI
    singolo file compilato quando si usa LV_CONF_INCLUDE_SIMPLE con PlatformIO:
    è un artefatto noto e innocuo del controllo di inclusione di lv_conf.h fatto
    da LVGL stesso — il nostro lv_conf.h viene comunque trovato/applicato
    correttamente (e i valori davvero critici passano comunque forzati via -D
    nei build_flags, indipendentemente da questo). Puramente cosmetico: non
    tocca nessuna logica, rimuove solo il rumore ripetuto nel log di build."""
    internal_h = os.path.join(project_dir, ".pio", "libdeps", build_env, "lvgl", "src", "lv_conf_internal.h")
    if not os.path.isfile(internal_h):
        _append_log(job_id, f"({internal_h} non ancora presente: nulla da silenziare per ora)")
        return
    with open(internal_h, "r") as f:
        content = f.read()
    marker = '#pragma message("Possible failure to include lv_conf.h, please read the comment in this file if you get errors")'
    if marker in content:
        content = content.replace(
            marker, '/* pragma disattivato da firmware_builder.py: messaggio innocuo, vedi commento lì */'
        )
        with open(internal_h, "w") as f:
            f.write(content)
        _append_log(job_id, "Silenziato il messaggio informativo (innocuo) di LVGL su lv_conf.h.")
    else:
        _append_log(job_id, "Il messaggio di LVGL non è nel punto atteso (versione diversa?): lasciato com'è.")


def _build_meta(target: str, params: Dict[str, Any]) -> Dict[str, Any]:
    """Flag della versione da conservare accanto al .bin (tabella della web UI).
    Password e chiavi NON si salvano: solo se c'erano."""
    meta: Dict[str, Any] = {"target": target, "built_at": int(time.time()), "date": time.strftime("%Y%m%d")}
    if target == "loader_full":
        meta["config_sd"] = bool(params.get("config_sd"))
        return meta
    if target == "gateway":
        meta["chip"] = "ESP32"
        return meta
    if target == "loader":
        return meta
    for key in ("comm_mode", "device_id", "fw_revision", "server_host", "server_port", "server_host_vpn",
                "vpn_hostname", "poll_interval_ms", "wifi_ssid"):
        if params.get(key) not in (None, ""):
            meta[key] = params.get(key)
    meta["fw_version"] = params.get("fw_version") or read_tracked_version()
    meta["config_storage_sd"] = bool(params.get("config_storage_sd")) and params.get("comm_mode") == "wifi_vpn"
    if params.get("comm_mode") == "wifi_vpn":
        meta["vpn_default_enabled"] = params.get("vpn_default_enabled", True) not in (False, "false", "0", 0)
    meta["has_wifi_password"] = bool(params.get("wifi_password"))
    meta["has_vpn_auth_key"] = bool(params.get("vpn_auth_key"))
    meta["has_vpn_auth_key_ephemeral"] = bool(params.get("vpn_auth_key_ephemeral"))
    return meta


def _run_build(job_id: str, target: str, params: Dict[str, Any]):
    # Calcolato subito, prima di qualunque tentativo: così il file di log ha lo
    # stesso nome base del .bin anche se poi la build fallisce (nessun .bin da
    # generare, ma il tentativo resta comunque tracciato con lo stesso nome che
    # avrebbe avuto).
    planned_filename = _build_merged_filename(target, params)
    try:
        if target == "loader":
            project_dir = _LOADER_ROOT
            build_env = _LOADER_BUILD_ENV
            _append_log(job_id, "Build del loader leggero di test WiFi (nessuna BSP/display "
                                 "richiesta, più rapida delle altre due build)...")
        elif target == "loader_full":
            project_dir = _LOADER_FULL_ROOT
            build_env = _LOADER_FULL_BUILD_ENV
            _append_log(job_id, "Build del loader \"pesante\" (schermo/touch/WiFi/Bluetooth/VPN)%s..."
                        % (", configurazione su microSD" if params.get("config_sd") else ""))
        elif target == "gateway":
            project_dir = _GATEWAY_ROOT
            build_env = _GATEWAY_BUILD_ENV
            _render_gateway_build(params)
            g_name, g_id = gateway_identity(params.get("gateway_name", ""))
            _append_log(job_id, f"Nome del gateway: {g_name} (id sul server: {g_id})")
            _append_log(job_id, "Build del gateway Bluetooth per ESP32 classico (la prima volta scarica anche "
                                 "la toolchain per ESP32 e la libreria ESP32-A2DP: qualche minuto in più)...")
        else:
            project_dir = _FIRMWARE_ROOT
            build_env = _MAIN_BUILD_ENV
            _append_log(job_id, "Genero device_config.h dai parametri forniti...")
            _render_main_config(params)
            _append_log(job_id, "Avvio build PlatformIO (la prima volta può richiedere qualche minuto "
                                 "per scaricare toolchain e librerie)...")

        # Pulizia SOLO di .pio/build/<env> (grafo di dipendenze e oggetti compilati)
        # prima di ogni tentativo: risolve un'incoerenza osservata nella Library
        # Dependency Finder di PlatformIO tra un run e l'altro (una libreria in
        # firmware/lib/ scoperta in un run, "dimenticata" nel successivo). NON
        # tocca .pio/libdeps/<env> (le librerie già scaricate): quella cache resta,
        # le build successive alla prima sono molto più veloci. Se in futuro cambi
        # una versione pinnata in library.json, usa il bottone "Pulisci cache
        # librerie" nella web UI (clear_dependency_cache) per forzare un ridownload.
        stale_build_dir = os.path.join(project_dir, ".pio", "build", build_env)
        if os.path.isdir(stale_build_dir):
            _append_log(job_id, f"Pulisco la build precedente ({stale_build_dir})...")
            shutil.rmtree(stale_build_dir, ignore_errors=True)

        if target in ("main", "loader_full"):
            # Risolviamo le dipendenze SENZA compilare, cosa che scarica/aggiorna
            # anche lvgl in .pio/libdeps/, cosa necessaria prima di poter
            # silenziare il suo pragma (altrimenti il file da modificare non
            # esiste ancora la primissima volta). Il loader leggero non usa lvgl,
            # quindi non ha bisogno di questo passaggio.
            _append_log(job_id, "Risolvo le dipendenze (nessuna compilazione ancora)...")
            _run_subprocess(job_id, ["pio", "pkg", "install"], cwd=project_dir)
            _patch_lvgl_pragma(project_dir, build_env, job_id)

        # Loader: l'opzione microSD passa come flag di compilazione (il loader non
        # ha un device_config.h). PLATFORMIO_BUILD_FLAGS si somma ai build_flags.
        extra_env = None
        if target == "loader_full":
            extra_env = {"PLATFORMIO_BUILD_FLAGS": "-DLOADER_CONFIG_SD=%d" % (1 if params.get("config_sd") else 0)}
        build_ok = _run_subprocess(job_id, ["pio", "run", "-e", build_env], cwd=project_dir, extra_env=extra_env)
        if not build_ok:
            if target in ("main", "loader_full"):
                _append_log(job_id, "Build fallita. Se l'errore riguarda display_init()/touch_init() "
                                     "non definite, controlla firmware/lib/ — vedi platformio.ini.")
            _finish(job_id, success=False)
            return

        build_output_dir = os.path.join(project_dir, ".pio", "build", build_env)
        bootloader = os.path.join(build_output_dir, "bootloader.bin")
        partitions = os.path.join(build_output_dir, "partitions.bin")
        firmware = os.path.join(build_output_dir, "firmware.bin")
        for path in (bootloader, partitions, firmware):
            if not os.path.exists(path):
                _append_log(job_id, f"File atteso non trovato dopo la build: {path}")
                _finish(job_id, success=False)
                return

        merged_path = firmware_store.resolve_path(planned_filename)

        _append_log(job_id, "Unisco bootloader + partizioni + app in un unico binario (merge_bin)...")
        chip, boot_offset = _CHIP.get(target, _DEFAULT_CHIP)
        merge_ok = _run_subprocess(job_id, [
            "esptool.py", "--chip", chip, "merge_bin", "-o", merged_path,
            boot_offset, bootloader,
            "0x8000", partitions,
            "0x10000", firmware,
        ])
        if not merge_ok:
            _finish(job_id, success=False)
            return

        # ELF con i simboli, accanto al .bin: la prossima build sovrascrive quello
        # in .pio/build, e senza non si potrebbero decodificare i crash di QUESTO
        # firmware (web UI: "Decodifica un crash").
        elf = os.path.join(build_output_dir, "firmware.elf")
        if os.path.exists(elf):
            try:
                firmware_store.save_elf(planned_filename, elf)
                _append_log(job_id, "Conservato firmware.elf per decodificare eventuali crash.")
            except OSError as exc:
                _append_log(job_id, f"ATTENZIONE: firmware.elf non conservato ({exc}): i crash non si potranno decodificare.")
        # Flag della versione (modalità, microSD, server, VPN...) per la tabella dei firmware.
        try:
            firmware_store.save_meta(planned_filename, _build_meta(target, params))
        except OSError as exc:
            _append_log(job_id, f"ATTENZIONE: dati della versione non salvati ({exc}).")
        _append_log(job_id, f"Fatto. Firmware pronto per il flashing: {planned_filename}")
        with _lock:
            if _current_job and _current_job["job_id"] == job_id:
                _current_job["output_filename"] = planned_filename
        _finish(job_id, success=True)

    except Exception as exc:
        logger.exception("Errore inatteso durante la build")
        _append_log(job_id, f"Errore inatteso: {exc}")
        _finish(job_id, success=False)
    finally:
        _persist_log(job_id, planned_filename)


def _persist_log(job_id: str, planned_filename: str):
    """Salva il log completo di questo tentativo su disco, con lo stesso nome
    base del .bin (generato o solo tentato) — vedi firmware_store.resolve_log_path.
    Va SEMPRE eseguito, successo o fallimento: un tentativo fallito è spesso
    proprio quello che serve poter riguardare in seguito."""
    with _lock:
        lines = list(_current_job["log"]) if _current_job and _current_job["job_id"] == job_id else []
    if not lines:
        return
    try:
        log_path = firmware_store.resolve_log_path(planned_filename)
        with open(log_path, "w") as f:
            f.write("\n".join(lines) + "\n")
    except OSError as exc:
        logger.warning("Impossibile salvare il log di build su disco: %s", exc)


def _finish(job_id: str, success: bool):
    with _lock:
        if _current_job and _current_job["job_id"] == job_id:
            _current_job["state"] = "success" if success else "error"
            _current_job["finished_at"] = time.time()
