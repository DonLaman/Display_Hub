"""
Configurazione dei gateway Bluetooth (ESP32 WROVER, A2DP verso le casse).

Ogni gateway ha un nome (device id) e un file INI in data/BT_setting/<id>.ini,
nello stesso stile del file del display (sezioni, "valore | valore"). Il gateway,
una volta online, scarica il suo INI via API e applica cassa e volume: il SERVER
comanda, il gateway esegue. Il WiFi invece resta locale al gateway (USB o
portale), quindi nell'INI le reti WiFi sono facoltative, utili solo se vuoi
precaricarle dal server.

Struttura INI (vedi anche struttura del file del display):

    [wifi]
    network = SSID | password        ; facoltativo, una riga per rete

    [bt]
    speaker = AA:BB:CC:DD:EE:FF | Nome cassa
    volume = 60                      ; 0..100

    [server]
    poll_interval_s = 30             ; ogni quanto il gateway ricontrolla la config

API (in routes.py):
    GET  /api/bt-gateway/config?device_id=<id>   -> l'INI (text/plain), letto dal gateway
    GET  /api/bt-gateway/<id>                     -> config come JSON (per la web UI)
    PUT  /api/bt-gateway/<id>                      -> aggiorna da JSON o da INI grezzo
    GET  /api/bt-gateway                           -> elenco dei gateway configurati
"""
import ipaddress
import json
import os
import re
import time
import urllib.error
import urllib.request
from typing import Any, Dict, List, Optional, Tuple

from app.core.ini_doc import IniDoc

_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "data", "BT_setting")
_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,48}$")
_MAC_RE = re.compile(r"^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$")


def valid_device_id(device_id: str) -> bool:
    return bool(device_id and _ID_RE.match(device_id))


def _path(device_id: str) -> str:
    return os.path.join(_DIR, f"{device_id}.ini")


def _default_ini() -> str:
    return ("[wifi]\n\n"
            "[bt]\n"
            "speaker = \n"
            "volume = 60\n"
            "; dispositivi conosciuti (uno per riga): known = aa:bb:cc:dd:ee:ff | Nome\n\n"
            "[youtube]\n"
            "; storico dei video aperti/eseguiti, il piu recente in alto\n"
            "; history = VIDEO_ID | Titolo\n"
            "; preferiti\n"
            "; favorite = VIDEO_ID | Titolo\n\n"
            "[server]\n"
            "poll_interval_s = 30\n")


def list_gateways() -> List[Dict[str, Any]]:
    if not os.path.isdir(_DIR):
        return []
    out = []
    for name in sorted(os.listdir(_DIR)):
        if name.endswith(".ini"):
            dev = name[:-4]
            out.append({"device_id": dev, "updated_at": os.path.getmtime(_path(dev))})
    return out


def read_ini(device_id: str) -> str:
    """Testo INI del gateway. Se non esiste, restituisce un modello vuoto (non lo salva)."""
    try:
        with open(_path(device_id), encoding="utf-8") as f:
            return f.read()
    except FileNotFoundError:
        return _default_ini()


def write_ini(device_id: str, text: str):
    os.makedirs(_DIR, exist_ok=True)
    tmp = _path(device_id) + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, _path(device_id))


def to_dict(device_id: str) -> Dict[str, Any]:
    """Config del gateway come JSON, per la web UI."""
    doc = IniDoc(read_ini(device_id))
    speaker_raw = doc.get("bt", "speaker", "") or ""
    mac, _, name = speaker_raw.partition("|")
    networks = []
    for entry in doc.get_all("wifi", "network"):
        ssid, _, _pw = entry.partition("|")
        networks.append(ssid.strip())  # non esponiamo la password nella vista JSON
    try:
        volume = int(doc.get("bt", "volume", "60") or 60)
    except ValueError:
        volume = 60
    try:
        poll = int(doc.get("server", "poll_interval_s", "30") or 30)
    except ValueError:
        poll = 30
    return {
        "device_id": device_id,
        "speaker_mac": mac.strip(),
        "speaker_name": name.strip(),
        "volume": max(0, min(100, volume)),
        "poll_interval_s": max(5, poll),
        "wifi_networks": networks,
        "known_devices": known_devices(device_id),
        "youtube_history": _yt_list(doc, "history"),
        "youtube_favorites": _yt_list(doc, "favorite"),
    }


def _yt_list(doc, key):
    """Righe [youtube] history/favorite come lista di {id, title}."""
    out = []
    for entry in doc.get_all("youtube", key):
        vid, _, title = entry.partition("|")
        if vid.strip():
            out.append({"id": vid.strip(), "title": title.strip()})
    return out


class ValidationError(ValueError):
    pass


def update_from_dict(device_id: str, data: Dict[str, Any]) -> Dict[str, Any]:
    """Aggiorna cassa, volume, poll (e opzionalmente WiFi) mantenendo il resto del file."""
    doc = IniDoc(read_ini(device_id))

    if "speaker_mac" in data or "speaker_name" in data:
        mac = str(data.get("speaker_mac", "")).strip()
        name = str(data.get("speaker_name", "")).strip()
        if mac and not _MAC_RE.match(mac):
            raise ValidationError("MAC della cassa non valido (formato AA:BB:CC:DD:EE:FF)")
        doc.set("bt", "speaker", f"{mac} | {name}" if (mac or name) else "")

    if "volume" in data:
        try:
            v = int(data["volume"])
        except (ValueError, TypeError):
            raise ValidationError("volume non valido (0-100)")
        doc.set("bt", "volume", str(max(0, min(100, v))))

    if "poll_interval_s" in data:
        try:
            p = int(data["poll_interval_s"])
        except (ValueError, TypeError):
            raise ValidationError("poll_interval_s non valido")
        doc.set("server", "poll_interval_s", str(max(5, p)))

    # WiFi opzionale: lista di {ssid, password}
    if "wifi_networks" in data and isinstance(data["wifi_networks"], list):
        entries = []
        for net in data["wifi_networks"]:
            ssid = str(net.get("ssid", "")).strip()
            pw = str(net.get("password", "")).strip()
            if ssid:
                entries.append(f"{ssid} | {pw}")
        doc.set_all("wifi", "network", entries)

    write_ini(device_id, doc.serialize())
    return to_dict(device_id)


def delete(device_id: str) -> bool:
    try:
        os.remove(_path(device_id))
        return True
    except FileNotFoundError:
        return False


# ---------------------------------------------------------------- comandi immediati al gateway
# Il gateway scarica la config ogni poll_interval_s (30 s): troppo lento per un tasto del
# volume. Quando la scarica, il server ne annota l'indirizzo (note_seen) e puo' poi mandargli
# i comandi subito con la sua API HTTP (push). Se il gateway non e' raggiungibile o ha un token,
# il push fallisce senza danni: la config resta nell'INI e viene applicata al giro dopo.
_seen: Dict[str, Tuple[str, float]] = {}
_PUSH_PORT = 80


def note_seen(device_id: str, ip: str) -> bool:
    """Annota da quale indirizzo un gateway ha scaricato la config. Solo reti locali."""
    try:
        addr = ipaddress.ip_address(ip)
    except ValueError:
        return False
    if not (addr.is_private or addr.is_loopback):
        return False
    _seen[device_id] = (str(addr), time.time())
    return True


def last_seen(device_id: str, max_age_s: int = 900) -> Optional[str]:
    seen = _seen.get(device_id)
    if not seen or time.time() - seen[1] > max_age_s:
        return None
    return seen[0]


def seen_ids() -> List[str]:
    """Gateway che hanno gia' scaricato la config da quando il server e' partito."""
    return sorted(_seen)


def call(device_id: str, method: str, path: str, body: Optional[Dict[str, Any]] = None,
         timeout: float = 2.0) -> Tuple[bool, Optional[Dict[str, Any]], str]:
    """Chiamata all'API HTTP del gateway. Ritorna (riuscita, JSON della risposta, errore).
    "Riuscita" = il gateway ha risposto 200: per i comandi guarda poi data["ok"]."""
    ip = last_seen(device_id)
    if not ip:
        return False, None, "gateway non ancora visto (non ha ancora scaricato la config)"
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(f"http://{ip}:{_PUSH_PORT}{path}", data=data, method=method,
                                 headers={"Content-Type": "application/json"} if data is not None else {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read()
            try:
                parsed = json.loads(raw.decode("utf-8") or "{}")
            except ValueError:
                parsed = {}
            return r.status == 200, parsed, ""
    except urllib.error.HTTPError as exc:
        return False, None, f"HTTP {exc.code}" + (" (il gateway richiede un token)" if exc.code == 401 else "")
    except Exception as exc:  # rete, timeout...
        return False, None, str(exc)


def push(device_id: str, path: str, body: Dict[str, Any], timeout: float = 2.0) -> Tuple[bool, str]:
    """POST JSON all'API del gateway. Ritorna (riuscito, messaggio d'errore)."""
    ok, _, err = call(device_id, "POST", path, body, timeout)
    return ok, err


# ---------------------------------------------------------------- dispositivi conosciuti
# Il gateway ricorda UNA sola cassa; l'elenco di quelle conosciute sta qui, nell'INI:
#   [bt]
#   speaker = aa:bb:cc:dd:ee:ff | Nome      ; quella da usare (vuota = nessuna)
#   known = aa:bb:cc:dd:ee:ff | Nome        ; una riga per dispositivo conosciuto
# Cosi' l'elenco resiste anche a un riflash del gateway.
def norm_mac(mac: str) -> str:
    m = (mac or "").strip().lower()
    if not _MAC_RE.match(m):
        raise ValidationError("indirizzo non valido (aa:bb:cc:dd:ee:ff)")
    return m


def _split_dev(entry: str) -> Dict[str, str]:
    mac, _, name = entry.partition("|")
    return {"addr": mac.strip().lower(), "name": name.strip()}


def known_devices(device_id: str) -> List[Dict[str, str]]:
    doc = IniDoc(read_ini(device_id))
    return [d for d in (_split_dev(e) for e in doc.get_all("bt", "known")) if _MAC_RE.match(d["addr"])]


def _write_known(device_id: str, doc, devices: List[Dict[str, str]]):
    doc.set_all("bt", "known", [f"{d['addr']} | {d['name']}" for d in devices])
    write_ini(device_id, doc.serialize())


def known_add(device_id: str, mac: str, name: str = ""):
    mac = norm_mac(mac)
    doc = IniDoc(read_ini(device_id))
    devices = [d for d in (_split_dev(e) for e in doc.get_all("bt", "known")) if _MAC_RE.match(d["addr"])]
    for d in devices:
        if d["addr"] == mac:
            if name:
                d["name"] = name.strip()      # un nome migliore sostituisce quello vecchio
            break
    else:
        devices.append({"addr": mac, "name": (name or "").strip()})
    _write_known(device_id, doc, devices)


def known_remove(device_id: str, mac: str) -> bool:
    mac = norm_mac(mac)
    doc = IniDoc(read_ini(device_id))
    devices = [d for d in (_split_dev(e) for e in doc.get_all("bt", "known")) if _MAC_RE.match(d["addr"])]
    left = [d for d in devices if d["addr"] != mac]
    if len(left) == len(devices):
        return False
    _write_known(device_id, doc, left)
    return True


def speaker(device_id: str) -> Dict[str, str]:
    mac, _, name = (IniDoc(read_ini(device_id)).get("bt", "speaker", "") or "").partition("|")
    return {"addr": mac.strip().lower(), "name": name.strip()}


def set_speaker(device_id: str, mac: str, name: str = ""):
    """Cassa da usare ("" = nessuna). Va tenuta d'accordo con quello che si comanda al gateway:
    il gateway applica questa riga a ogni download della config."""
    doc = IniDoc(read_ini(device_id))
    doc.set("bt", "speaker", f"{norm_mac(mac)} | {(name or '').strip()}" if mac else "")
    write_ini(device_id, doc.serialize())
