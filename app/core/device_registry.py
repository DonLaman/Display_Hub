"""
Tiene traccia dei display che comunicano via WiFi (nessuna seriale coinvolta
in questo flusso). Un display si registra con /api/esp/hello al boot e ad
ogni /api/esp/poll aggiorna il proprio "last_seen": la web UI usa questi dati
per mostrare se il display è online, il suo IP e la versione firmware.

In memoria: alla scala di un solo display (o pochi) non serve persistenza,
e comunque un riavvio del container coincide quasi sempre con un riavvio
in cui il display si ri-registra da solo entro pochi secondi.
"""
import threading
import time
from typing import Any, Dict

_LOCK = threading.Lock()
_devices: Dict[str, Dict[str, Any]] = {}

OFFLINE_THRESHOLD_SECONDS = 30  # se non fa poll da più di così, lo consideriamo offline


def register(device_id: str, ip: str, fw_version: str = ""):
    with _LOCK:
        existing = _devices.get(device_id, {})
        _devices[device_id] = {
            "device_id": device_id,
            "ip": ip,
            "fw_version": fw_version or existing.get("fw_version", ""),
            "last_seen": time.time(),
            "first_seen": existing.get("first_seen", time.time()),
        }


def touch(device_id: str, ip: str):
    """Aggiorna last_seen/ip senza richiedere di nuovo la fw_version (usato dal poll)."""
    with _LOCK:
        if device_id in _devices:
            _devices[device_id]["last_seen"] = time.time()
            _devices[device_id]["ip"] = ip
        else:
            _devices[device_id] = {
                "device_id": device_id, "ip": ip, "fw_version": "",
                "last_seen": time.time(), "first_seen": time.time(),
            }


def list_devices():
    with _LOCK:
        now = time.time()
        return [
            {
                **info,
                "online": (now - info["last_seen"]) < OFFLINE_THRESHOLD_SECONDS,
                "seconds_since_seen": round(now - info["last_seen"], 1),
            }
            for info in _devices.values()
        ]


def forget(device_id: str) -> bool:
    """Toglie un display dall'elenco (es. un vecchio Device ID che non esiste più).
    Se è ancora acceso, ricomparirà al suo prossimo poll."""
    with _LOCK:
        return _devices.pop(device_id, None) is not None
