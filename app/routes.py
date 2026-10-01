"""
Tre blueprint:
- ui_bp:  pagine HTML (web UI)
- api_bp: usate dal BROWSER via fetch() -> configurazione pagine, catalogo widget,
          gestione USB/flashing, elenco display WiFi connessi
- esp_bp: usate dal DISPLAY via WiFi -> registrazione e polling dati.
          Nessuna autenticazione qui di proposito: si presume rete fidata
          raggiungibile solo via VPN (vedi nota sicurezza nel README).
"""
import logging
import os

from flask import Blueprint, jsonify, render_template, request, send_file, send_from_directory

from app.core import tv_vidaa
from app.core import bt_gateway
from app.core import (audio_player, gateway_control, changelog, config_store, crash_decoder, device_config, device_registry, docs, firmware_builder,
                      firmware_store)
from app.core.orchestrator import data_cache
from app.core.registry import registry

logger = logging.getLogger(__name__)

ui_bp = Blueprint("ui", __name__)
api_bp = Blueprint("api", __name__)
esp_bp = Blueprint("esp", __name__)


# ================= UI =================

@ui_bp.route("/")
def index():
    # Versione e revisione proposte nel form di compilazione: quelle della voce
    # più recente del registro "Versioni" (restano modificabili nel form).
    cur = changelog.current() or {}
    return render_template("index.html",
                           current_fw_version=cur.get("version") or firmware_builder.read_tracked_version(),
                           current_fw_revision=cur.get("revision", ""))


# ================= API: documentazione (file .md come HTML) =================

@api_bp.route("/docs", methods=["GET"])
def docs_list():
    return jsonify(docs.list_docs())


@api_bp.route("/docs/<doc_id>", methods=["GET"])
def docs_get(doc_id):
    doc = docs.render(doc_id)
    if not doc:
        return jsonify({"error": "documento non trovato"}), 404
    return jsonify(doc)


# ================= API: registro delle versioni =================

def _changelog_payload():
    entries = changelog.list_entries()
    out = [dict(e, notes_html=docs.render_markdown(e.get("notes", ""))) for e in entries]
    cur = entries[0] if entries else None
    return {"entries": out,
            "current": {"id": cur["id"], "version": cur["version"], "revision": cur["revision"]} if cur else None,
            "next_revision": changelog.next_revision(cur["revision"]) if cur else "rev_1"}


@api_bp.route("/changelog", methods=["GET"])
def changelog_list():
    return jsonify(_changelog_payload())


@api_bp.route("/changelog", methods=["POST"])
def changelog_create():
    try:
        changelog.create(request.json or {})
    except changelog.ValidationError as exc:
        return jsonify({"error": str(exc)}), 400
    return jsonify(_changelog_payload())


@api_bp.route("/changelog/<entry_id>", methods=["PUT", "DELETE"])
def changelog_edit(entry_id):
    if request.method == "DELETE":
        if not changelog.delete(entry_id):
            return jsonify({"error": "voce non trovata"}), 404
        return jsonify(_changelog_payload())
    try:
        if not changelog.update(entry_id, request.json or {}):
            return jsonify({"error": "voce non trovata"}), 404
    except changelog.ValidationError as exc:
        return jsonify({"error": str(exc)}), 400
    return jsonify(_changelog_payload())


# ================= API: browser -> catalogo/config pagine =================

@api_bp.route("/widgets", methods=["GET"])
def list_widgets():
    result = []
    for meta in registry.list_meta():
        widget = registry.get(meta.id)
        result.append({
            "id": meta.id,
            "name": meta.name,
            "description": meta.description,
            "icon": meta.icon,
            "layout_type": meta.layout_type,
            "refresh_seconds": meta.refresh_seconds,
            "config_schema": widget.get_config_schema(),
        })
    return jsonify(result)


@api_bp.route("/widgets/<widget_id>/preview", methods=["POST"])
def preview_widget(widget_id):
    try:
        registry.get(widget_id)
    except KeyError:
        return jsonify({"error": "widget non trovato"}), 404
    # Anteprima con i parametri scelti nella web UI (body JSON {"params": {...}}):
    # calcolata al volo, non tocca i dati delle pagine del display.
    params = (request.json or {}).get("params", {}) if request.is_json else {}
    try:
        return jsonify(data_cache.preview(widget_id, params))
    except Exception as exc:
        return jsonify({"error": f"calcolo fallito: {exc}"}), 500


@api_bp.route("/pages", methods=["GET"])
def get_pages():
    return jsonify(config_store.get_pages())


@api_bp.route("/pages", methods=["POST"])
def set_pages():
    pages = request.json
    if not isinstance(pages, list):
        return jsonify({"error": "atteso un array di pagine"}), 400
    for page in pages:
        if not isinstance(page, dict) or "widget_id" not in page:
            return jsonify({"error": "ogni pagina deve avere widget_id"}), 400
        if "params" in page and not isinstance(page["params"], dict):
            return jsonify({"error": "params deve essere un oggetto"}), 400
        try:
            registry.get(page["widget_id"])
        except KeyError:
            return jsonify({"error": f"widget_id sconosciuto: {page['widget_id']}"}), 400
    config_store.set_pages(pages)
    data_cache.apply_config_change()  # incrementa config_version: il display lo vedrà al prossimo poll
    return jsonify({"status": "ok", "pages": pages, "config_version": data_cache.config_version})


# ================= API: browser -> dispositivi WiFi connessi =================

@api_bp.route("/devices", methods=["GET"])
def list_devices():
    """Display visti da questo avvio del server, più quelli di cui c'è solo la
    configurazione salvata (mai visti da questo avvio): known_only = True."""
    devices = device_registry.list_devices()
    seen = {d["device_id"] for d in devices}
    configs = {c["device_id"]: c for c in device_config.list_all()}
    for d in devices:
        d["has_config"] = d["device_id"] in configs
    for dev_id, c in configs.items():
        if dev_id not in seen:
            devices.append({"device_id": dev_id, "ip": "", "fw_version": "", "online": False,
                            "known_only": True, "has_config": True, "config_updated_at": c["updated_at"]})
    devices.sort(key=lambda d: (not d["online"], d["device_id"].lower()))
    return jsonify(devices)


@api_bp.route("/devices/<device_id>", methods=["DELETE"])
def forget_device(device_id):
    """Dimentica un display; con ?config=1 cancella anche la sua configurazione salvata."""
    if not device_config.valid_device_id(device_id):
        return jsonify({"error": "device_id non valido"}), 400
    removed = device_registry.forget(device_id)
    config_removed = device_config.delete(device_id) if request.args.get("config") in ("1", "true") else False
    return jsonify({"status": "ok", "removed": removed, "config_removed": config_removed})


# ================= API: browser -> firmware (upload/elenco) + flashing via Web Serial =================

# ================= API: browser -> configurazione dei display =================

@api_bp.route("/device-configs", methods=["GET"])
def device_configs_list():
    return jsonify(device_config.list_all())


@api_bp.route("/devices/<device_id>/config", methods=["GET", "PUT"])
def device_config_api(device_id):
    """GET: configurazione del display salvata sul server (404 se non c'è ancora).
    PUT {"base_rev", "text"}: salva; 409 con la versione attuale se nel frattempo
    è cambiata (dal display o da un'altra scheda della web UI)."""
    if not device_config.valid_device_id(device_id):
        return jsonify({"error": "device_id non valido"}), 400
    if request.method == "GET":
        rec = device_config.get(device_id)
        if not rec:
            return jsonify({"error": "nessuna configurazione ancora ricevuta da questo display", "rev": 0}), 404
        return jsonify(rec)
    body = request.json or {}
    text = body.get("text")
    if not isinstance(text, str):
        return jsonify({"error": "text mancante"}), 400
    try:
        result = device_config.save_from_web(device_id, int(body.get("base_rev") or 0), text)
    except device_config.ConflictError as exc:
        return jsonify({"error": str(exc), "current": exc.current}), 409
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 400
    logger.info("Configurazione del display %s modificata dalla web UI -> rev %d", device_id, result["rev"])
    return jsonify(result)


@api_bp.route("/firmware", methods=["GET"])
def firmware_list():
    return jsonify(firmware_store.list_firmware_files())


@api_bp.route("/firmware", methods=["POST"])
def firmware_upload():
    if "file" not in request.files:
        return jsonify({"error": "nessun file ricevuto (campo 'file' mancante)"}), 400
    try:
        filename = firmware_store.save_uploaded_firmware(request.files["file"])
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 400
    return jsonify({"status": "ok", "filename": filename})


@api_bp.route("/firmware/<filename>", methods=["DELETE"])
def firmware_delete(filename):
    firmware_store.delete_firmware(filename)  # .bin + ELF + log + dati della versione
    return jsonify({"status": "ok"})


@api_bp.route("/firmware/delete-many", methods=["POST"])
def firmware_delete_many():
    names = (request.json or {}).get("filenames") or []
    if not isinstance(names, list):
        return jsonify({"error": "filenames deve essere un elenco"}), 400
    n = sum(1 for name in names if isinstance(name, str) and firmware_store.delete_firmware(name))
    return jsonify({"status": "ok", "deleted": n})


@api_bp.route("/firmware/<filename>/log", methods=["GET"])
def firmware_log(filename):
    path = firmware_store.resolve_log_path(filename)
    if not os.path.exists(path):
        return jsonify({"error": "log non disponibile"}), 404
    return send_file(path, mimetype="text/plain; charset=utf-8")


@api_bp.route("/firmware/orphan-logs", methods=["GET", "DELETE"])
def firmware_orphan_logs():
    """Log di build senza firmware (build fallite, firmware già cancellati)."""
    if request.method == "DELETE":
        return jsonify({"status": "ok", "deleted": firmware_store.delete_orphan_logs()})
    return jsonify(firmware_store.list_orphan_logs())


@api_bp.route("/firmware/decode", methods=["POST"])
def firmware_decode():
    """Decodifica un crash incollato dalla console seriale.
    Body {"log": "...", "firmware": "<nome .bin>" (facoltativo, se il log non ha
    'ELF file SHA256')}. Vedi app/core/crash_decoder.py."""
    body = request.get_json(silent=True) or {}
    text = body.get("log") or ""
    if len(text) > 200_000:
        return jsonify({"error": "log troppo lungo: incolla solo la parte del crash"}), 400
    try:
        return jsonify(crash_decoder.decode(text, body.get("firmware") or None))
    except (ValueError, LookupError) as exc:
        return jsonify({"error": str(exc)}), 400
    except RuntimeError as exc:
        return jsonify({"error": str(exc)}), 503


@api_bp.route("/firmware/<filename>/manifest.json", methods=["GET"])
def firmware_manifest(filename):
    """
    Manifest nel formato atteso da ESP Web Tools (<esp-web-install-button>):
    il browser lo legge per sapere quale file scaricare e a quale offset
    scriverlo via Web Serial. Qui si assume un unico .bin "merged" (bootloader
    + partizioni + app uniti con `esptool.py merge_bin`) scritto a 0x0 — vedi
    PROTOCOL.md per come generarlo dal tuo toolchain PlatformIO/Arduino.
    """
    if not firmware_store.exists(filename):
        return jsonify({"error": "firmware non trovato"}), 404
    return jsonify({
        "name": filename,
        "version": "1.0.0",
        "builds": [
            {
                # Il gateway Bluetooth gira su un ESP32 classico, tutto il resto su ESP32-S3.
                "chipFamily": "ESP32" if firmware_store.is_gateway_file(filename) else "ESP32-S3",
                "parts": [
                    {"path": f"/api/firmware/{filename}/bin", "offset": 0}
                ],
            }
        ],
    })


@api_bp.route("/firmware/<filename>/bin", methods=["GET"])
def firmware_binary(filename):
    """Serve il .bin grezzo: è il file a cui punta il manifest sopra."""
    return send_from_directory(firmware_store.firmware_dir(), filename, mimetype="application/octet-stream")


# ================= API: browser -> build firmware server-side =================

@api_bp.route("/firmware/build", methods=["POST"])
def firmware_build_start():
    body = request.json or {}
    try:
        job_id = firmware_builder.start_build(body)
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 400
    except RuntimeError as exc:
        return jsonify({"error": str(exc)}), 409
    return jsonify({"status": "started", "job_id": job_id})


@api_bp.route("/firmware/build-loader", methods=["POST"])
def firmware_build_loader_start():
    """Build del loader leggero di test WiFi: nessun parametro richiesto dal body."""
    try:
        job_id = firmware_builder.start_loader_build()
    except RuntimeError as exc:
        return jsonify({"error": str(exc)}), 409
    return jsonify({"status": "started", "job_id": job_id})


@api_bp.route("/firmware/build-loader-full", methods=["POST"])
def firmware_build_loader_full_start():
    """Build del loader "pesante" (schermo/touch/WiFi/Bluetooth/VPN). Body
    facoltativo: {"config_sd": true} = configurazione su microSD."""
    body = request.get_json(silent=True) or {}
    try:
        job_id = firmware_builder.start_loader_full_build(config_sd=bool(body.get("config_sd")))
    except RuntimeError as exc:
        return jsonify({"error": str(exc)}), 409
    return jsonify({"status": "started", "job_id": job_id})


@api_bp.route("/firmware/build-gateway", methods=["POST"])
def firmware_build_gateway_start():
    """Build del gateway Bluetooth (ESP32 classico, A2DP verso le casse)."""
    name = str((request.get_json(silent=True) or {}).get("name", "")).strip()
    try:
        job_id = firmware_builder.start_gateway_build(name)
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 400
    except RuntimeError as exc:
        return jsonify({"error": str(exc)}), 409
    return jsonify({"status": "started", "job_id": job_id})


@api_bp.route("/firmware/build/status", methods=["GET"])
def firmware_build_status():
    status = firmware_builder.get_status()
    if status is None:
        return jsonify({"state": "idle"})
    return jsonify(status)


@api_bp.route("/firmware/clear-cache", methods=["POST"])
def firmware_clear_cache():
    """Cancella la cache delle librerie scaricate (.pio/libdeps) per 'main' o 'loader',
    forzando un ridownload completo alla prossima build. Le build normali non la toccano
    più da sole: usa questo solo se sospetti che una versione di libreria vada aggiornata."""
    body = request.json or {}
    target = body.get("target")
    if target not in ("main", "loader", "loader_full", "gateway"):
        return jsonify({"error": "target deve essere 'main', 'loader', 'loader_full' o 'gateway'"}), 400
    try:
        message = firmware_builder.clear_dependency_cache(target)
    except RuntimeError as exc:
        return jsonify({"error": str(exc)}), 409
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 400
    return jsonify({"status": "ok", "message": message})


# ================= API: display (WiFi) =================

@esp_bp.route("/tv-action", methods=["POST"])
def esp_tv_action():
    """Azione del telecomando dal DISPLAY (canale /api/esp, senza login).
    Stessa logica di /api/tv/action, ma esente da autenticazione perche' il
    display non puo' fare il login della web app."""
    action = (request.json or {}).get("action", "")
    kind, _, arg = action.partition(":")
    if kind == "key" and arg.startswith("KEY_"):
        return _tv_call(tv_vidaa.send_key, arg)
    if kind == "power":
        return _tv_call(tv_vidaa.send_key, "KEY_POWER")
    return jsonify({"ok": False, "error": f"azione non valida: {action}"}), 400


@esp_bp.route("/audio-action", methods=["POST"])
def esp_audio_action():
    """Azione del player audio dal DISPLAY (canale /api/esp, senza login: il display non puo'
    farlo). ?gateway=<id> sceglie il gateway (dal widget); vedi core/audio_player.py."""
    gid = audio_player.resolve_gateway(request.args.get("gateway"))
    action = (request.get_json(silent=True) or {}).get("action", "")
    try:
        result = audio_player.handle_action(action, gid)
    except audio_player.NotAvailable as exc:
        return jsonify({"ok": False, "placeholder": True, "error": str(exc)}), 501
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400
    return jsonify({"ok": True, **result})


@esp_bp.route("/bt-gateway-config", methods=["GET"])
def esp_bt_gateway_config():
    """Config (INI) del gateway Bluetooth, scaricata DAL GATEWAY. Sta sotto /api/esp perche' il
    gateway non puo' fare il login della web app (sotto /api/bt-gateway riceveva 401). Annota
    anche da dove ha scaricato, per mandargli i comandi subito (volume)."""
    device_id = request.args.get("device_id", "")
    if not bt_gateway.valid_device_id(device_id):
        return "device_id non valido", 400, {"Content-Type": "text/plain; charset=utf-8"}
    bt_gateway.note_seen(device_id, request.remote_addr or "")
    return bt_gateway.read_ini(device_id), 200, {"Content-Type": "text/plain; charset=utf-8"}


# ---- Impostazioni > Gateway Bluetooth del display (stesso canale /api/esp: senza login, solo diretto) ----
def _gw_id(gid):
    return gid if bt_gateway.valid_device_id(gid) else None


@esp_bp.route("/bt-gateways", methods=["GET"])
def esp_bt_gateways():
    return jsonify({"ok": True, "gateways": gateway_control.list_gateways()})


@esp_bp.route("/bt-gateways/<gid>", methods=["GET"])
def esp_bt_gateway_info(gid):
    if not _gw_id(gid):
        return jsonify({"ok": False, "error": "id non valido"}), 400
    return jsonify({"ok": True, **gateway_control.info(gid)})


@esp_bp.route("/bt-gateways/<gid>/scan", methods=["GET", "POST"])
def esp_bt_gateway_scan(gid):
    if not _gw_id(gid):
        return jsonify({"ok": False, "error": "id non valido"}), 400
    if request.method == "POST":
        ok, err = gateway_control.scan_start(gid)
        return jsonify({"ok": ok, "error": err}), (200 if ok else 502)
    return jsonify(gateway_control.scan_state(gid))


def _gw_action(gid, fn, *args):
    if not _gw_id(gid):
        return jsonify({"ok": False, "error": "id non valido"}), 400
    try:
        ok, err = fn(gid, *args)
    except bt_gateway.ValidationError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400
    return jsonify({"ok": ok, "error": err}), (200 if ok else 502)


@esp_bp.route("/bt-gateways/<gid>/connect", methods=["POST"])
def esp_bt_gateway_connect(gid):
    body = request.get_json(silent=True) or {}
    return _gw_action(gid, gateway_control.connect, body.get("addr", ""), str(body.get("name", ""))[:60])


@esp_bp.route("/bt-gateways/<gid>/disconnect", methods=["POST"])
def esp_bt_gateway_disconnect(gid):
    return _gw_action(gid, gateway_control.disconnect)


@esp_bp.route("/bt-gateways/<gid>/forget", methods=["POST"])
def esp_bt_gateway_forget(gid):
    return _gw_action(gid, gateway_control.forget, (request.get_json(silent=True) or {}).get("addr", ""))


@esp_bp.route("/hello", methods=["POST"])
def esp_hello():
    """Il display chiama questo endpoint al boot, dopo essersi connesso al WiFi."""
    body = request.json or {}
    device_id = body.get("device_id", "unknown")
    fw_version = body.get("fw_version", "")
    ip = request.remote_addr
    device_registry.register(device_id, ip, fw_version)
    logger.info("Display connesso via WiFi: %s (%s) fw=%s", device_id, ip, fw_version)
    return jsonify({"status": "ok"})


@esp_bp.route("/poll", methods=["GET"])
def esp_poll():
    """
    Chiamata periodica del display (consigliato ogni 3-5s): ritorna in un colpo solo
    tutte le pagine attive con i rispettivi dati già pronti (calcolati in background
    da data_cache, mai in questa richiesta). Il display confronta config_version con
    quella che ha già per capire se deve ricostruire gli schermi.
    """
    device_id = request.args.get("device_id", "unknown")
    fw_version = request.args.get("fw_version", "")
    # register(), non touch(): se fw_version arriva vuota conserva quella già
    # nota, ma se il singolo /hello al boot fosse andato perso (es. intoppo di
    # rete transitorio) questo la recupera comunque al primo poll successivo
    # che la riporta — altrimenti restava "?" nell'elenco per tutta la sessione.
    device_registry.register(device_id, request.remote_addr, fw_version)
    snapshot = dict(data_cache.get_snapshot())  # copia: il campo sotto è per QUESTO display
    # Revisione della configurazione del display conservata sul server: se è
    # diversa da quella con cui è allineato, il display chiama /api/esp/config.
    snapshot["cfg_rev"] = device_config.current_rev(device_id) if device_config.valid_device_id(device_id) else 0
    return jsonify(snapshot)


@esp_bp.route("/config", methods=["GET", "POST"])
def esp_config():
    """Sincronizzazione della configurazione del display (displayhub.txt).

    POST {"device_id", "base_rev", "text"}: text = configurazione attuale del
    display se ha modifiche locali (o null per scaricare soltanto); base_rev =
    ultima revisione del server con cui era allineato. Risposta {"rev", "text"}:
    la versione con cui allinearsi (unione a tre vie se è cambiata anche qui).
    GET ?device_id=...: solo lettura. Vedi app/core/device_config.py.
    """
    if request.method == "GET":
        device_id = request.args.get("device_id", "")
        base_rev, text = 0, None
    else:
        body = request.json or {}
        device_id = body.get("device_id", "")
        base_rev = int(body.get("base_rev") or 0)
        text = body.get("text")
    if not device_config.valid_device_id(device_id):
        return jsonify({"error": "device_id non valido"}), 400
    if text is not None and not isinstance(text, str):
        return jsonify({"error": "text deve essere una stringa"}), 400
    try:
        result = device_config.sync_from_device(device_id, base_rev, text)
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 400
    if text is not None:
        logger.info("Configurazione dal display %s (base rev %d) -> rev %d", device_id, base_rev, result["rev"])
    return jsonify(result)


# ================= API: TV Hisense VIDAA =================

def _tv_call(fn, *args):
    try:
        out = fn(*args)
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400
    except RuntimeError as exc:
        return jsonify({"ok": False, "error": str(exc), "status": tv_vidaa.status()}), 502
    return jsonify({"ok": True, "result": out, "status": tv_vidaa.status()})


@api_bp.route("/tv", methods=["GET", "PUT"])
def tv_config():
    if request.method == "PUT":
        tv_vidaa.update_config(request.json or {})
    return jsonify(tv_vidaa.status())


@api_bp.route("/tv/connect", methods=["POST"])
def tv_connect():
    return _tv_call(lambda: (tv_vidaa.TV.ensure(), tv_vidaa.request_state()) and None)


@api_bp.route("/tv/key", methods=["POST"])
def tv_key():
    return _tv_call(tv_vidaa.send_key, (request.json or {}).get("key", ""))


@api_bp.route("/tv/pair/start", methods=["POST"])
def tv_pair_start():
    return _tv_call(tv_vidaa.pair_start)


@api_bp.route("/tv/pair/confirm", methods=["POST"])
def tv_pair_confirm():
    return _tv_call(tv_vidaa.pair_confirm, str((request.json or {}).get("code", "")))


@api_bp.route("/tv/diagnose", methods=["POST"])
def tv_diagnose():
    return _tv_call(tv_vidaa.diagnose)


@api_bp.route("/tv/power", methods=["POST"])
def tv_power():
    """action: "toggle" (default, KEY_POWER accende/spegne), "on" (Wake-on-LAN), "off" (KEY_POWER)."""
    body = request.json or {}
    action = body.get("action") or ("on" if body.get("on") else "toggle")
    if action == "on":
        cfg = tv_vidaa.load_config()
        return _tv_call(tv_vidaa.wake, cfg.get("mac", ""), cfg.get("ip", ""))
    # toggle e off usano entrambi KEY_POWER (sulla VIDAA e' un interruttore)
    return _tv_call(tv_vidaa.send_key, "KEY_POWER")


@api_bp.route("/tv/action", methods=["POST"])
def tv_action():
    """Azione dal telecomando touch del DISPLAY. Formato: {"action": "key:KEY_OK"}
    o {"action": "power:toggle"}. Le action "view:*" sono gestite dal display e
    non arrivano qui."""
    action = (request.json or {}).get("action", "")
    kind, _, arg = action.partition(":")
    if kind == "key":
        if not arg.startswith("KEY_"):
            return jsonify({"ok": False, "error": "tasto non valido"}), 400
        return _tv_call(tv_vidaa.send_key, arg)
    if kind == "power":
        return _tv_call(tv_vidaa.send_key, "KEY_POWER")
    return jsonify({"ok": False, "error": f"azione sconosciuta: {action}"}), 400


# ========================= Gateway Bluetooth (WROVER) =========================
# Config di cassa e volume comandata dal server. Il gateway, una volta online,
# scarica il suo INI e applica. WiFi resta locale al gateway (USB o portale).

@api_bp.route("/bt-gateway", methods=["GET"])
def bt_gateway_list():
    return jsonify(bt_gateway.list_gateways())


@api_bp.route("/bt-gateway/config", methods=["GET"])
def bt_gateway_config_ini():
    """Letta DAL GATEWAY: restituisce l'INI grezzo da applicare."""
    device_id = request.args.get("device_id", "")
    if not bt_gateway.valid_device_id(device_id):
        return "device_id non valido", 400, {"Content-Type": "text/plain; charset=utf-8"}
    return bt_gateway.read_ini(device_id), 200, {"Content-Type": "text/plain; charset=utf-8"}


@api_bp.route("/bt-gateway/<device_id>", methods=["GET", "PUT", "DELETE"])
def bt_gateway_config(device_id):
    if not bt_gateway.valid_device_id(device_id):
        return jsonify({"error": "device_id non valido"}), 400
    if request.method == "DELETE":
        return jsonify({"status": "ok", "deleted": bt_gateway.delete(device_id)})
    if request.method == "PUT":
        # accetta JSON (campi) oppure INI grezzo (Content-Type text/plain)
        if request.content_type and "text/plain" in request.content_type:
            bt_gateway.write_ini(device_id, request.get_data(as_text=True))
        else:
            try:
                bt_gateway.update_from_dict(device_id, request.json or {})
            except bt_gateway.ValidationError as exc:
                return jsonify({"error": str(exc)}), 400
    return jsonify(bt_gateway.to_dict(device_id))
