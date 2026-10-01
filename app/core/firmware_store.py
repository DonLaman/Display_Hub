"""
Gestione dei file firmware caricati dalla web UI. Nessun accesso USB qui:
il flashing avviene nel browser via Web Serial (ESP Web Tools), il server
si limita a conservare i .bin e a generare il "manifest" che la libreria
lato client usa per sapere cosa scrivere e a quale offset.

Vedi PROTOCOL.md per il formato del manifest.
"""
import glob
import json
import os
import re
import time
from typing import Any, Dict, List

_FIRMWARE_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "data", "firmware")


def _ensure_dir():
    os.makedirs(_FIRMWARE_DIR, exist_ok=True)


def firmware_dir() -> str:
    _ensure_dir()
    return _FIRMWARE_DIR


def list_firmware_files() -> List[Dict[str, Any]]:
    _ensure_dir()
    files = []
    for path in sorted(glob.glob(os.path.join(_FIRMWARE_DIR, "*.bin")), key=os.path.getmtime, reverse=True):
        stat = os.stat(path)
        filename = os.path.basename(path)
        files.append({
            "filename": filename,
            "size_bytes": stat.st_size,
            "uploaded_at": stat.st_mtime,
            # Il builder nomina i loader "<timestamp>_loader.bin" (leggero) o
            # "<timestamp>_loaderfull.bin" (con schermo/WiFi/BT): distinguerli
            # permette alla web UI di separarli nell'elenco.
            # (anche "_loaderfull-sd.bin": loader con configurazione su microSD)
            "kind": (
                "gateway" if is_gateway_file(filename)
                else "loader_full" if "_loaderfull" in filename
                else "loader" if filename.endswith("_loader.bin")
                else "main"
            ),
            # ELF conservato: serve a decodificare i crash di questo firmware.
            "has_elf": os.path.exists(resolve_elf_path(filename)),
            "has_log": os.path.exists(resolve_log_path(filename)),
            # Com'è stato costruito (modalità, microSD, server, VPN...): vedi firmware_meta().
            "meta": firmware_meta(filename),
        })
    return files


def save_uploaded_firmware(file_storage) -> str:
    """Salva un .bin caricato dalla web UI. Ritorna il nome file salvato."""
    _ensure_dir()
    filename = file_storage.filename
    if not filename.endswith(".bin"):
        raise ValueError("Il file firmware deve avere estensione .bin")
    safe_name = f"{int(time.time())}_{os.path.basename(filename)}"
    dest = os.path.join(_FIRMWARE_DIR, safe_name)
    file_storage.save(dest)
    return safe_name


def delete_firmware(filename: str) -> bool:
    """Cancella il firmware e tutto ciò che lo accompagna: ELF, log di build, dati della versione."""
    _ensure_dir()
    removed = False
    for path in (os.path.join(_FIRMWARE_DIR, os.path.basename(filename)), resolve_elf_path(filename),
                 resolve_log_path(filename), resolve_meta_path(filename)):
        if os.path.exists(path):
            os.remove(path)
            removed = True
    return removed


def resolve_path(filename: str) -> str:
    """Path assoluto di un firmware, per servirlo come file statico al browser."""
    _ensure_dir()
    return os.path.join(_FIRMWARE_DIR, os.path.basename(filename))


def exists(filename: str) -> bool:
    return os.path.exists(resolve_path(filename))


_LOGS_DIR = os.path.join(_FIRMWARE_DIR, "logs")


def logs_dir() -> str:
    """Cartella dei log di build, uno per ogni tentativo (riuscito o no), con lo
    stesso nome base del .bin generato o che si stava tentando di generare."""
    os.makedirs(_LOGS_DIR, exist_ok=True)
    return _LOGS_DIR


def resolve_log_path(merged_filename: str) -> str:
    base = os.path.basename(merged_filename)
    if base.endswith(".bin"):
        base = base[: -len(".bin")]
    return os.path.join(logs_dir(), f"{base}.log")


# ---------------------------------------------------------------- ELF
# Per ogni firmware generato dal builder si conserva anche firmware.elf (con i
# simboli di debug): serve a tradurre gli indirizzi di un crash ("Backtrace:
# 0x42...") in funzione/file/riga. Il firmware stampa "ELF file SHA256: <hash>"
# nel crash: così si riconosce quale ELF usare (vedi crash_decoder.py).
_ELF_DIR = os.path.join(_FIRMWARE_DIR, "elf")


def elf_dir() -> str:
    os.makedirs(_ELF_DIR, exist_ok=True)
    return _ELF_DIR


def resolve_elf_path(merged_filename: str) -> str:
    base = os.path.basename(merged_filename)
    if base.endswith(".bin"):
        base = base[: -len(".bin")]
    return os.path.join(_ELF_DIR, f"{base}.elf")


def save_elf(merged_filename: str, source_elf: str) -> str:
    import shutil
    elf_dir()
    dest = resolve_elf_path(merged_filename)
    shutil.copyfile(source_elf, dest)
    return dest


def list_elfs() -> List[Dict[str, Any]]:
    """ELF conservati, dal più recente: [{"path", "firmware"}]."""
    out = []
    for path in sorted(glob.glob(os.path.join(elf_dir(), "*.elf")), key=os.path.getmtime, reverse=True):
        out.append({"path": path, "firmware": os.path.basename(path)[: -len(".elf")] + ".bin"})
    return out


# ---------------------------------------------------------------- dati della versione
# Per ogni firmware generato il builder salva i parametri con cui l'ha
# costruito ("flag" della versione): modalità, microSD, server, VPN, rete...
# MAI password o chiavi: solo se c'erano (has_*). Per i firmware precedenti a
# questa funzione (o caricati a mano) si ricava quello che si può dal nome.
_META_DIR = os.path.join(_FIRMWARE_DIR, "meta")


def resolve_meta_path(merged_filename: str) -> str:
    base = os.path.basename(merged_filename)
    if base.endswith(".bin"):
        base = base[: -len(".bin")]
    return os.path.join(_META_DIR, f"{base}.json")


def save_meta(merged_filename: str, meta: Dict[str, Any]):
    os.makedirs(_META_DIR, exist_ok=True)
    with open(resolve_meta_path(merged_filename), "w") as f:
        json.dump(meta, f, indent=1)


# <modalità>[-sd]-<AAAAMMGG>-<versione>[-<revisione>]_<device_id>.bin
# Versione numerica (3.2.0); la revisione può avere UN prefisso con "_"
# (rev_7, hotfix_2) o essere semplice (r1). Il resto dopo "_" è il device_id,
# che può contenere a sua volta "_" (Dash_Final).
_MAIN_NAME = re.compile(r"^(usb|wifi_vpn|wifi)(-sd)?-(\d{8})-(\d+(?:\.\d+)*)"
                        r"(?:-((?:[A-Za-z]+_)?[A-Za-z0-9.]+))?_(.+)\.bin$")
_MAIN_NAME_LOOSE = re.compile(r"^(usb|wifi_vpn|wifi)(-sd)?-(\d{8})-([^_]+)_(.+)\.bin$")


# <timestamp>_gateway.bin, oppure <timestamp>_gateway-<id>.bin se alla build e' stato scelto un nome
_GATEWAY_NAME = re.compile(r"^(\d{9,11})_gateway(?:-([A-Za-z0-9_.-]+))?\.bin$")


def is_gateway_file(filename: str) -> bool:
    return bool(_GATEWAY_NAME.match(os.path.basename(filename)))


def _meta_from_filename(filename: str) -> Dict[str, Any]:
    m = _GATEWAY_NAME.match(filename)
    if m:
        meta = {"target": "gateway", "chip": "ESP32", "built_at": int(m.group(1)), "source": "nome del file"}
        if m.group(2):
            meta["gateway_id"] = m.group(2)
        return meta
    m = re.match(r"^(\d{9,11})_(loaderfull(-sd)?|loader)\.bin$", filename)
    if m:
        return {"target": "loader_full" if m.group(2).startswith("loaderfull") else "loader",
                "config_sd": bool(m.group(3)), "built_at": int(m.group(1)), "source": "nome del file"}
    m = _MAIN_NAME.match(filename)
    if m:
        return {"target": "main", "comm_mode": m.group(1), "config_storage_sd": bool(m.group(2)),
                "date": m.group(3), "fw_version": m.group(4), "fw_revision": m.group(5) or "",
                "device_id": m.group(6), "source": "nome del file"}
    m = _MAIN_NAME_LOOSE.match(filename)  # versione non numerica (es. "beta")
    if m:
        version, _, revision = m.group(4).partition("-")
        return {"target": "main", "comm_mode": m.group(1), "config_storage_sd": bool(m.group(2)),
                "date": m.group(3), "fw_version": version, "fw_revision": revision,
                "device_id": m.group(5), "source": "nome del file"}
    return {"target": "sconosciuto", "source": "caricato a mano"}


def firmware_meta(filename: str) -> Dict[str, Any]:
    try:
        with open(resolve_meta_path(filename)) as f:
            meta = json.load(f)
            meta.setdefault("source", "build")
            return meta
    except (FileNotFoundError, json.JSONDecodeError):
        return _meta_from_filename(os.path.basename(filename))


# ---------------------------------------------------------------- log orfani
def list_orphan_logs() -> List[Dict[str, Any]]:
    """Log di build senza firmware (build fallite o firmware già cancellati)."""
    out = []
    for path in sorted(glob.glob(os.path.join(logs_dir(), "*.log")), key=os.path.getmtime, reverse=True):
        base = os.path.basename(path)[: -len(".log")]
        if not os.path.exists(os.path.join(_FIRMWARE_DIR, base + ".bin")):
            out.append({"name": os.path.basename(path), "size_bytes": os.path.getsize(path),
                        "modified_at": os.path.getmtime(path)})
    return out


def delete_orphan_logs() -> int:
    n = 0
    for item in list_orphan_logs():
        os.remove(os.path.join(logs_dir(), item["name"]))
        n += 1
    return n
