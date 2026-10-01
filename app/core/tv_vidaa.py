"""
Connettore TV Hisense VIDAA basato sulla libreria vidaa-control.

Sostituisce l'implementazione MQTT scritta a mano (in tv_vidaa_manual_backup.py):
la libreria porta i certificati client corretti e gestisce l'autenticazione
"dynamic" dei firmware recenti (VIDAA U7/OS7). Verificato sul campo: pairing con
PIN e comandi funzionanti.

Interfaccia pubblica INVARIATA (routes.py e pannello web non cambiano):
    load_config(), save_config(), update_config(data)
    status()
    send_key(key)            key in stile "KEY_VOLUMEUP" (come manda la web UI)
    pair_start()             fa comparire il PIN sulla TV
    pair_confirm(code)       invia il PIN, salva il token
    request_state()
    wake(mac, ip)
    diagnose()
    TV.ensure()

Config in data/tv.json; token gestito da vidaa-control in data/vidaa_tokens.json.
"""
import json
import logging
import os
import threading
import time
from typing import Any, Dict, List

logger = logging.getLogger(__name__)

_DATA_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "data")
_CONFIG = os.path.join(_DATA_DIR, "tv.json")
_TOKEN_FILE = os.path.join(_DATA_DIR, "vidaa_tokens.json")  # TokenStorage vuole un FILE

_KEY_ALIASES = {"KEY_RETURN": "KEY_RETURNS", "KEY_BACK": "KEY_RETURNS"}


class ValidationError(ValueError):
    pass


def _default_config() -> Dict[str, Any]:
    return {"name": "TV", "ip": "", "mac": "", "paired": False}


def load_config() -> Dict[str, Any]:
    try:
        with open(_CONFIG, encoding="utf-8") as f:
            cfg = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        cfg = {}
    base = _default_config()
    base.update(cfg or {})
    return base


def save_config(cfg: Dict[str, Any]):
    os.makedirs(_DATA_DIR, exist_ok=True)
    tmp = _CONFIG + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=1)
    os.replace(tmp, _CONFIG)


def update_config(data: Dict[str, Any]) -> Dict[str, Any]:
    cfg = load_config()
    before = (cfg.get("ip"),)
    for k in ("name", "ip", "mac"):
        if k in data and isinstance(data[k], str):
            cfg[k] = data[k].strip()
    if (cfg.get("ip"),) != before:
        cfg["paired"] = False
        _tv.reset()
    save_config(cfg)
    return cfg


def _is_connected(client) -> bool:
    try:
        v = client.is_connected
        return v() if callable(v) else bool(v)
    except Exception:
        return False


def _is_authenticated(client) -> bool:
    try:
        return bool(client.is_authenticated())
    except Exception:
        return False


class _TvClient:
    def __init__(self):
        self._lock = threading.Lock()
        self._client = None
        self.connected = False
        self.last_error = ""
        self.state: Dict[str, Any] = {}
        self.state_at = 0.0

    def reset(self):
        with self._lock:
            if self._client is not None:
                try:
                    self._client.disconnect()
                except Exception:
                    pass
            self._client = None
            self.connected = False

    def _make(self):
        from vidaa import VidaaTV, TokenStorage
        cfg = load_config()
        if not cfg.get("ip"):
            raise RuntimeError("indirizzo IP della TV non impostato")
        from pathlib import Path
        os.makedirs(_DATA_DIR, exist_ok=True)
        storage = TokenStorage(storage_path=Path(_TOKEN_FILE))
        return VidaaTV(host=cfg["ip"], mac_address=cfg.get("mac") or None,
                       auto_detect_protocol=True, use_dynamic_auth=True, storage=storage)

    def ensure(self, timeout: float = 15.0, pairing: bool = False):
        with self._lock:
            if self._client is not None and self.connected:
                return self._client
            client = self._make()
            try:
                client.connect(timeout=timeout, auto_auth=not pairing)
            except Exception as exc:
                self.last_error = f"connessione non riuscita ({exc})"
                self.connected = _is_connected(client)
                if not (pairing and self.connected) and not self.connected:
                    raise RuntimeError(self.last_error)
            self._client = client
            self.connected = _is_connected(client)
            if self.connected:
                self.last_error = ""
            return client

    def get(self):
        return self._client


_tv = _TvClient()
TV = _tv  # alias storico usato da routes.py


def send_key(key: str):
    key = (key or "").strip().upper()
    key = _KEY_ALIASES.get(key, key)
    if not key.startswith("KEY_"):
        raise ValidationError(f"tasto non valido: {key}")
    client = _tv.ensure()
    try:
        ok = client.send_key(key)
    except Exception as exc:
        _tv.connected = False
        raise RuntimeError(f"invio tasto non riuscito ({exc})")
    if not ok:
        raise RuntimeError("la TV non ha accettato il comando (token scaduto? riprova l'abbinamento)")
    return {"key": key, "sent": True}


def pair_start():
    client = _tv.ensure(pairing=True)
    try:
        started = client.start_pairing()
    except Exception as exc:
        raise RuntimeError(f"avvio abbinamento non riuscito ({exc})")
    if not started:
        raise RuntimeError("la TV non ha avviato l'abbinamento")
    return {"pairing": True}


def pair_confirm(code: str, timeout: float = 15.0) -> bool:
    code = (code or "").strip()
    if not (code.isdigit() and 4 <= len(code) <= 6):
        raise ValidationError("codice di 4 cifre")
    client = _tv.get() or _tv.ensure(pairing=True)
    try:
        ok = client.authenticate(code, timeout=timeout)
    except Exception as exc:
        raise RuntimeError(f"abbinamento non riuscito ({exc})")
    if ok and _is_authenticated(client):
        cfg = load_config()
        cfg["paired"] = True
        save_config(cfg)
        _tv.connected = True
        return True
    return False


def request_state():
    client = _tv.ensure()
    try:
        st = client.get_state(timeout=5.0)
        if isinstance(st, dict):
            _tv.state = st
            _tv.state_at = time.time()
    except Exception:
        pass


def status() -> Dict[str, Any]:
    cfg = load_config()
    client = _tv.get()
    authed = _is_authenticated(client) if client else False
    return {
        "name": cfg.get("name", "TV"),
        "ip": cfg.get("ip", ""),
        "mac": cfg.get("mac", ""),
        "paired": cfg.get("paired", False) or authed,
        "connected": _tv.connected,
        "authenticated": authed,
        "last_error": _tv.last_error,
        "state": _tv.state,
        "state_age_s": int(time.time() - _tv.state_at) if _tv.state_at else None,
        "backend": "vidaa-control",
    }


def wake(mac: str, ip: str = "") -> str:
    mac = (mac or "").strip()
    if len(mac.replace(":", "").replace("-", "")) != 12:
        raise ValidationError("MAC della TV non valido")
    try:
        from vidaa import wol
        wol.send_magic_packet(mac)
        return f"Wake-on-LAN inviato a {mac}"
    except Exception:
        pass
    import socket
    hexmac = mac.replace(":", "").replace("-", "").lower()
    packet = bytes.fromhex("ff" * 6 + hexmac * 16)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        for target in (ip.rsplit(".", 1)[0] + ".255" if ip.count(".") == 3 else "255.255.255.255",
                       "255.255.255.255"):
            for port in (9, 7):
                s.sendto(packet, (target, port))
    return f"Wake-on-LAN inviato a {mac} (broadcast)"


def diagnose() -> List[Dict[str, Any]]:
    cfg = load_config()
    out = [{"prova": "IP configurato", "risultato": cfg.get("ip") or "(nessuno)"}]
    try:
        client = _tv.ensure(pairing=True)
        out.append({"prova": "connessione TLS", "risultato": "OK" if _is_connected(client) else "fallita"})
        out.append({"prova": "autenticato", "risultato": "sì" if _is_authenticated(client) else "no (serve abbinamento)"})
    except Exception as exc:
        out.append({"prova": "connessione", "risultato": f"errore: {exc}"})
    return out
