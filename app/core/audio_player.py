"""
Stato e comandi del player audio (gateway Bluetooth).

Per ora e' VERO solo il volume (nell'INI del gateway + invio immediato al gateway se lo
raggiunge). Le fonti (MP3 / YouTube / Web radio) sono SEGNAPOSTO: si puo' sceglierne una, ma la
riproduzione non e' ancora sviluppata, e play/pausa/precedente/successivo/stop rispondono
"non ancora disponibile" (NotAvailable -> HTTP 501) invece di fingere.

Le azioni arrivano dal display come stringhe: "vol:up|down|mute", "source:mp3|youtube|radio",
"audio:play_pause|prev|next|stop". Un'azione puo' contenere piu' parti separate da ";": quelle
"view:..." sono del display (cambio schermata) e qui si ignorano.
"""
import threading
from typing import Any, Dict

from app.core import bt_gateway

SOURCES = {"mp3": "MP3", "youtube": "YouTube", "radio": "Web radio"}
DEFAULT_GATEWAY = "gateway-01"     # lo stesso id predefinito del firmware del gateway
VOLUME_STEP = 5
HEADER_H = 100                     # altezza della scheda "in riproduzione" sul display
_PLAYBACK = ("play_pause", "prev", "next", "stop")

_lock = threading.Lock()
_state: Dict[str, Dict[str, Any]] = {}


class NotAvailable(Exception):
    """Funzione prevista ma non ancora sviluppata."""


def resolve_gateway(requested: Any) -> str:
    """Gateway da pilotare: quello richiesto, altrimenti il primo configurato, altrimenti il predefinito."""
    if requested and bt_gateway.valid_device_id(str(requested)):
        return str(requested)
    gws = bt_gateway.list_gateways()
    return gws[0]["device_id"] if gws else DEFAULT_GATEWAY


def _st(gid: str) -> Dict[str, Any]:
    with _lock:
        return _state.setdefault(gid, {"source": None, "premute": None})


def header(gid: str) -> Dict[str, Any]:
    """Contenuto della scheda in testa al player."""
    st = _st(gid)
    vol = bt_gateway.to_dict(gid)["volume"]
    vol_txt = "MUTO" if st["premute"] is not None else f"Vol {vol}%"
    src = st["source"]
    if src:
        label = SOURCES[src]
        return {"height": HEADER_H, "badge": f"{label.upper()} - {vol_txt}", "title": "Nessun brano",
                "subtitle": f"{label}: riproduzione non ancora disponibile", "progress": -1}
    return {"height": HEADER_H, "badge": vol_txt, "title": "Nessuna riproduzione",
            "subtitle": "Scegli una fonte con il pulsante Fonti", "progress": -1}


def set_volume(gid: str, value: int) -> Dict[str, Any]:
    v = max(0, min(100, int(value)))
    bt_gateway.update_from_dict(gid, {"volume": v})   # memorizzato: vale anche dopo un riavvio
    ok, err = bt_gateway.push(gid, "/api/audio/volume", {"volume": v})
    return {"volume": v, "pushed": ok, "push_error": err or None}


def _volume_action(gid: str, arg: str) -> Dict[str, Any]:
    cur = bt_gateway.to_dict(gid)["volume"]
    st = _st(gid)
    if arg == "up":
        new, st["premute"] = min(100, cur + VOLUME_STEP), None
    elif arg == "down":
        new, st["premute"] = max(0, cur - VOLUME_STEP), None
    elif arg == "mute":
        if st["premute"] is None:                      # muto: ricorda il livello
            st["premute"], new = (cur if cur > 0 else 40), 0
        else:                                          # secondo tocco: lo ripristina
            new, st["premute"] = st["premute"], None
    else:
        raise ValueError(f"azione volume non valida: {arg}")
    return set_volume(gid, new)


def handle_action(action: str, gid: str) -> Dict[str, Any]:
    if not (action or "").strip():
        raise ValueError("azione vuota")
    result: Dict[str, Any] = {}
    for part in action.split(";"):
        part = part.strip()
        if not part or part.startswith("view:"):
            continue
        kind, _, arg = part.partition(":")
        if kind == "vol":
            result.update(_volume_action(gid, arg))
        elif kind == "source":
            if arg not in SOURCES:
                raise ValueError(f"fonte sconosciuta: {arg}")
            _st(gid)["source"] = arg
            result.update({"source": arg, "placeholder": True})
        elif kind == "audio" and arg in _PLAYBACK:
            raise NotAvailable("la riproduzione non e' ancora sviluppata (fonti segnaposto)")
        else:
            raise ValueError(f"azione non valida: {part}")
    return result
