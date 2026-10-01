"""
Persistenza semplice su file JSON per la configurazione delle pagine attive.
Volutamente senza DB: alla scala di questo progetto (un display, poche decine
di pagine possibili) un file è più che sufficiente e facilissimo da ispezionare
o resettare a mano (data/config.json).
"""
import json
import os
import threading
import uuid
from typing import Any, Dict, List

_LOCK = threading.Lock()
_CONFIG_PATH = os.path.join(os.path.dirname(__file__), "..", "..", "data", "config.json")

_DEFAULT_CONFIG = {
    "pages": [],          # lista ordinata di {"page_id": ..., "widget_id": ..., "params": {...}}
}


def _ensure_file():
    os.makedirs(os.path.dirname(_CONFIG_PATH), exist_ok=True)
    if not os.path.exists(_CONFIG_PATH):
        with open(_CONFIG_PATH, "w") as f:
            json.dump(_DEFAULT_CONFIG, f, indent=2)


def load_config() -> Dict[str, Any]:
    _ensure_file()
    with _LOCK:
        with open(_CONFIG_PATH, "r") as f:
            return json.load(f)


def save_config(config: Dict[str, Any]):
    _ensure_file()
    with _LOCK:
        with open(_CONFIG_PATH, "w") as f:
            json.dump(config, f, indent=2)


def _assign_page_ids(pages: List[Dict[str, Any]]) -> bool:
    """Ogni pagina ha un identificativo proprio ("page_id"): lo stesso widget può
    comparire più volte con parametri diversi (es. due prezzi crypto) e ognuna
    ha i suoi dati. Assegna quelli mancanti o duplicati; True se ha cambiato qualcosa."""
    seen = set()
    changed = False
    for p in pages:
        pid = p.get("page_id")
        if not pid or not isinstance(pid, str) or pid in seen:
            pid = uuid.uuid4().hex[:8]
            while pid in seen:
                pid = uuid.uuid4().hex[:8]
            p["page_id"] = pid
            changed = True
        seen.add(pid)
        if not isinstance(p.get("params"), dict):
            p["params"] = {}
            changed = True
    return changed


def get_pages() -> List[Dict[str, Any]]:
    config = load_config()
    pages = config.get("pages", [])
    if _assign_page_ids(pages):  # configurazioni precedenti: identificativi assegnati una volta
        config["pages"] = pages
        save_config(config)
    return pages


def set_pages(pages: List[Dict[str, Any]]):
    _assign_page_ids(pages)
    config = load_config()
    config["pages"] = pages
    save_config(config)
