"""
Registro delle versioni e revisioni del firmware (web UI: "Versioni").

Ogni voce: versione, revisione, data, titolo, note (Markdown). La più in alto
è la "versione attuale": il form di compilazione propone la sua versione e
revisione (restano modificabili). File: data/changelog.json.

Al primo avvio il registro nasce con una voce per la situazione attuale:
versione da firmware/VERSION, revisione dall'ultimo firmware nell'archivio.
"""
import json
import os
import re
import threading
import time
import uuid
from typing import Any, Dict, List, Optional

_LOCK = threading.Lock()
_PATH = os.path.join(os.path.dirname(__file__), "..", "..", "data", "changelog.json")
_TOKEN = re.compile(r"^[A-Za-z0-9._-]{0,32}$")
_DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


class ValidationError(ValueError):
    pass


def _load() -> List[Dict[str, Any]]:
    try:
        with open(_PATH) as f:
            data = json.load(f)
            return data.get("entries", [])
    except FileNotFoundError:
        return None
    except json.JSONDecodeError:
        return []


def _save(entries: List[Dict[str, Any]]):
    os.makedirs(os.path.dirname(_PATH), exist_ok=True)
    tmp = _PATH + ".tmp"
    with open(tmp, "w") as f:
        json.dump({"entries": entries}, f, indent=1, ensure_ascii=False)
    os.replace(tmp, _PATH)


def _seed() -> List[Dict[str, Any]]:
    from app.core import firmware_builder, firmware_store
    version = firmware_builder.read_tracked_version()
    revision = ""
    for f in firmware_store.list_firmware_files():  # dal più recente
        meta = f.get("meta") or {}
        if meta.get("target") == "main" and meta.get("fw_version"):
            version = meta.get("fw_version") or version
            revision = meta.get("fw_revision") or ""
            break
    now = time.time()
    return [{
        "id": uuid.uuid4().hex[:8], "version": version, "revision": revision,
        "date": time.strftime("%Y-%m-%d"), "title": "Situazione attuale",
        "notes": "Voce creata automaticamente con la versione dell'ultimo firmware generato.\n\n"
                 "Aggiungi qui le novità di ogni revisione: la voce più in alto è quella che il "
                 "form di compilazione propone.",
        "created_at": now, "updated_at": now,
    }]


def list_entries() -> List[Dict[str, Any]]:
    with _LOCK:
        entries = _load()
        if entries is None:
            entries = _seed()
            _save(entries)
        return entries


def current() -> Optional[Dict[str, Any]]:
    entries = list_entries()
    return entries[0] if entries else None


def next_revision(rev: str) -> str:
    """rev_7 -> rev_8, r1 -> r2, vuota -> rev_1, hotfix -> hotfix2."""
    m = re.match(r"^(.*?)(\d+)$", rev or "")
    if not rev:
        return "rev_1"
    if m:
        return f"{m.group(1)}{int(m.group(2)) + 1}"
    return f"{rev}2"


def _clean(data: Dict[str, Any], partial: bool = False) -> Dict[str, Any]:
    out = {}
    if not partial or "version" in data:
        v = str(data.get("version") or "").strip()
        if not v or not _TOKEN.match(v):
            raise ValidationError("versione obbligatoria: lettere, numeri, . _ - (max 32)")
        out["version"] = v
    if not partial or "revision" in data:
        r = str(data.get("revision") or "").strip()
        if not _TOKEN.match(r):
            raise ValidationError("revisione: lettere, numeri, . _ - (max 32)")
        out["revision"] = r
    if not partial or "date" in data:
        d = str(data.get("date") or time.strftime("%Y-%m-%d")).strip()
        if not _DATE.match(d):
            raise ValidationError("data nel formato AAAA-MM-GG")
        out["date"] = d
    if not partial or "title" in data:
        out["title"] = str(data.get("title") or "").strip()[:120]
    if not partial or "notes" in data:
        notes = str(data.get("notes") or "")
        if len(notes) > 20000:
            raise ValidationError("note troppo lunghe (max 20000 caratteri)")
        out["notes"] = notes
    return out


def create(data: Dict[str, Any]) -> Dict[str, Any]:
    entry = _clean(data)
    now = time.time()
    entry.update({"id": uuid.uuid4().hex[:8], "created_at": now, "updated_at": now})
    with _LOCK:
        entries = _load() or []
        entries.insert(0, entry)  # la nuova diventa la versione attuale
        _save(entries)
    return entry


def update(entry_id: str, data: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    changes = _clean(data, partial=True)
    with _LOCK:
        entries = _load() or []
        for e in entries:
            if e["id"] == entry_id:
                e.update(changes)
                e["updated_at"] = time.time()
                _save(entries)
                return e
    return None


def delete(entry_id: str) -> bool:
    with _LOCK:
        entries = _load() or []
        kept = [e for e in entries if e["id"] != entry_id]
        if len(kept) == len(entries):
            return False
        _save(kept)
        return True
