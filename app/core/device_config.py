"""
Configurazione di ogni display (il suo displayhub.txt) conservata sul server e
sincronizzata col display durante il polling.

Perché: il server fa da copia di riserva (un display senza microSD riparte
dalla configurazione salvata qui) e da punto di modifica dalla web UI.

Versioni: ogni configurazione ha un numero di revisione "rev" che cresce a ogni
modifica (dal display o dalla web UI). Il display ricorda l'ultima rev con cui
è allineato ("base_rev") e la manda insieme alle sue modifiche.

Unione a tre vie (merge_three_way): se nel frattempo è cambiata anche la
copia del server (es. modifica dalla web UI), per ogni voce:
  - l'ha cambiata il display rispetto alla base -> vince il display
    (è la modifica più recente: arriva adesso);
  - altrimenti -> resta quella del server.
Base sconosciuta (display mai sincronizzato, es. microSD nuova creata con i
valori della build, o base uscita dallo storico): sui singoli valori vince il
SERVER (la copia fatta dall'utente, non i valori predefiniti della build); gli
elenchi si uniscono senza perdere voci (a parità di voce, quella del server).
Gli elenchi (reti WiFi, dispositivi e chiavi BT) si uniscono voce per voce
(identità = testo prima di '|'), l'ordine è quello di chi l'ha cambiato.
Il testo del display fa da base del risultato: i suoi commenti restano.

La sezione [sync] è del display (vi annota la rev) e non viene mai unita.
Password e chiavi viaggiano in entrambe le direzioni (scelta esplicita): le
API non hanno autenticazione, come il resto dell'app (rete fidata / VPN).

File: data/devices/<device_id>.json
  {"rev": 7, "text": "...", "updated_at": 1727..., "source": "device|web",
   "history": {"6": "...", "5": "..."}}          (ultime HISTORY_KEEP versioni)
"""
import json
import os
import re
import threading
import time
from typing import Any, Dict, List, Optional, Tuple

from app.core.ini_doc import IniDoc, is_list_key, list_id

_LOCK = threading.Lock()
_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "data", "devices")
HISTORY_KEEP = 30
MAX_TEXT = 64 * 1024
LOCAL_SECTIONS = {"sync"}  # sezioni che restano del display, mai sincronizzate

_ID_RE = re.compile(r"^[a-zA-Z0-9_-]{1,32}$")
_rev_cache: Dict[str, int] = {}  # device_id -> rev: il polling non rilegge il file ogni volta


class ConflictError(Exception):
    """La web UI ha modificato una versione vecchia: va ricaricata."""

    def __init__(self, current: Dict[str, Any]):
        super().__init__("configurazione cambiata nel frattempo")
        self.current = current


def valid_device_id(device_id: str) -> bool:
    return bool(device_id and _ID_RE.match(device_id))


def strip_local(text: str) -> str:
    doc = IniDoc(text)
    for s in LOCAL_SECTIONS:
        doc.remove_section(s)
    return doc.serialize()


# ---------------------------------------------------------------- unione
def _pairs(*docs: IniDoc) -> List[Tuple[str, str]]:
    out: List[Tuple[str, str]] = []
    for d in docs:
        for s in d.sections():
            if s in LOCAL_SECTIONS:
                continue
            for k in d.keys(s):
                if (s, k) not in out:
                    out.append((s, k))
    return out


def _list_map(doc: IniDoc, s: str, k: str) -> Tuple[Dict[str, str], List[str]]:
    m: Dict[str, str] = {}
    order: List[str] = []
    for e in doc.get_all(s, k):
        if not e.strip():
            continue
        i = list_id(e)
        if i not in m:
            m[i] = e
            order.append(i)
    return m, order


def merge_three_way(base_text: Optional[str], server_text: str, device_text: str) -> str:
    """Unisce le modifiche del display (device) con la copia del server.

    base_text None (base sconosciuta): vince il server sui singoli valori, gli
    elenchi si uniscono (nessuna voce persa).
    """
    base = IniDoc(base_text) if base_text is not None else None
    server = IniDoc(server_text)
    result = IniDoc(device_text)  # base del risultato: formato e commenti del display

    for s, k in _pairs(server, result, *( [base] if base else [] )):
        if is_list_key(s, k):
            dm, dorder = _list_map(result, s, k)
            sm, sorder = _list_map(server, s, k)
            if base is not None:
                bm, border = _list_map(base, s, k)
            else:
                bm, border = sm, sorder  # senza base: il display "ha cambiato" solo ciò che differisce
            merged: Dict[str, Optional[str]] = {}
            for i in set(dm) | set(sm) | set(bm):
                d, sv, b = dm.get(i), sm.get(i), bm.get(i)
                if base is None:
                    merged[i] = sv if sv is not None else d  # unione: nessuna voce persa
                else:
                    merged[i] = d if d != b else sv
            order_src = sorder if base is None else (dorder if dorder != border else sorder)
            other = sorder if order_src is dorder else dorder
            order = [i for i in order_src if merged.get(i)] + [i for i in other if merged.get(i) and i not in order_src]
            order += [i for i in merged if merged[i] and i not in order]
            values = [merged[i] for i in order]
            if values != result.get_all(s, k):
                if values or result.has(s, k) or server.has(s, k):
                    result.set_all(s, k, values if values else [""])
            continue

        d = result.get(s, k) if result.has(s, k) else None
        sv = server.get(s, k) if server.has(s, k) else None
        if base is None:
            chosen = sv if sv is not None else d
        else:
            b = base.get(s, k) if base.has(s, k) else None
            chosen = d if d != b else sv
        if chosen is None:
            result.remove(s, k)
        elif chosen != d:
            result.set(s, k, chosen)
    return result.serialize()


# ---------------------------------------------------------------- archivio
def _path(device_id: str) -> str:
    return os.path.join(_DIR, device_id + ".json")


def _load(device_id: str) -> Optional[Dict[str, Any]]:
    try:
        with open(_path(device_id), "r") as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return None


def _save(device_id: str, rec: Dict[str, Any]):
    _rev_cache[device_id] = rec["rev"]
    os.makedirs(_DIR, exist_ok=True)
    tmp = _path(device_id) + ".tmp"
    with open(tmp, "w") as f:
        json.dump(rec, f, indent=1)
    os.replace(tmp, _path(device_id))  # scrittura atomica


def _new_rev(rec: Optional[Dict[str, Any]], text: str, source: str) -> Dict[str, Any]:
    history = dict((rec or {}).get("history", {}))
    if rec:
        history[str(rec["rev"])] = rec["text"]
    keep = sorted(history, key=int)[-HISTORY_KEEP:]
    return {
        "rev": (rec["rev"] + 1) if rec else 1,
        "text": text,
        "updated_at": time.time(),
        "source": source,
        "history": {k: history[k] for k in keep},
    }


def get(device_id: str) -> Optional[Dict[str, Any]]:
    with _LOCK:
        rec = _load(device_id)
    if not rec:
        return None
    return {k: rec[k] for k in ("rev", "text", "updated_at", "source")}


def current_rev(device_id: str) -> int:
    with _LOCK:
        if device_id not in _rev_cache:
            rec = _load(device_id)
            _rev_cache[device_id] = rec["rev"] if rec else 0
        return _rev_cache[device_id]


def list_all() -> List[Dict[str, Any]]:
    out = []
    with _LOCK:
        if os.path.isdir(_DIR):
            for name in sorted(os.listdir(_DIR)):
                if name.endswith(".json"):
                    rec = _load(name[:-5])
                    if rec:
                        out.append({"device_id": name[:-5], "rev": rec["rev"], "updated_at": rec["updated_at"],
                                    "source": rec["source"]})
    return out


def sync_from_device(device_id: str, base_rev: int, text: Optional[str]) -> Dict[str, Any]:
    """Chiamata dal display. text None = nessuna modifica locale (solo scaricare).

    Ritorna {"rev": n, "text": "..."}: la configurazione con cui il display deve
    allinearsi (uguale al suo testo se nessuno l'ha cambiata nel frattempo).
    """
    with _LOCK:
        rec = _load(device_id)
        if text is None:
            if not rec:
                return {"rev": 0, "text": None}
            return {"rev": rec["rev"], "text": rec["text"]}
        if len(text) > MAX_TEXT:
            raise ValueError("configurazione troppo grande")
        text = strip_local(text)
        if not rec:
            rec = _new_rev(None, text, "device")
            _save(device_id, rec)
            return {"rev": rec["rev"], "text": rec["text"]}
        if base_rev == rec["rev"]:
            merged = text  # nessuno l'ha cambiata sul server: vale quella del display
        else:
            base = rec.get("history", {}).get(str(base_rev)) if base_rev > 0 else None
            merged = merge_three_way(base, rec["text"], text)
        if merged == rec["text"]:
            return {"rev": rec["rev"], "text": rec["text"]}
        rec = _new_rev(rec, merged, "device")
        _save(device_id, rec)
        return {"rev": rec["rev"], "text": rec["text"]}


def save_from_web(device_id: str, base_rev: int, text: str) -> Dict[str, Any]:
    """Modifica dalla web UI: accettata solo se basata sull'ultima versione."""
    if len(text) > MAX_TEXT:
        raise ValueError("configurazione troppo grande")
    text = strip_local(text)
    with _LOCK:
        rec = _load(device_id)
        cur = rec["rev"] if rec else 0
        if base_rev != cur:
            raise ConflictError({"rev": cur, "text": rec["text"] if rec else ""})
        if rec and rec["text"] == text:
            return {"rev": rec["rev"], "text": text}
        rec = _new_rev(rec, text, "web")
        _save(device_id, rec)
        return {"rev": rec["rev"], "text": rec["text"]}


def delete(device_id: str) -> bool:
    """Cancella la copia della configurazione di un display (e il suo storico)."""
    with _LOCK:
        _rev_cache.pop(device_id, None)
        try:
            os.remove(_path(device_id))
            return True
        except FileNotFoundError:
            return False
