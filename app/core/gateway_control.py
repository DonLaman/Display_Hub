"""
Pilotaggio dei gateway Bluetooth dal display (Impostazioni > Gateway Bluetooth).

Il display non parla direttamente col gateway: passa dal server (anche quando e' collegato in VPN),
che gira i comandi all'API HTTP del gateway e tiene l'elenco dei dispositivi conosciuti nell'INI
(vedi bt_gateway.py). Il gateway deve essere stato visto (aver scaricato la config) da quando il
server e' partito: ogni 30 s lo fa da solo.

Regola: i comandi che cambiano la connessione (connetti, disconnetti, dimentica la cassa in uso)
richiedono il gateway raggiungibile e cambiano l'INI SOLO se il gateway ha eseguito: cosi' l'INI
(che il gateway applica a ogni download) non resta in disaccordo con quello che sta facendo.
"""
from concurrent.futures import ThreadPoolExecutor
from typing import Any, Dict, List, Tuple

from app.core import bt_gateway

SCAN_SECONDS = 12
_PROBE_TIMEOUT = 1.5


def probe(gid: str) -> Dict[str, Any]:
    """Stato di un gateway: online con le sue info, oppure offline."""
    ok, st, err = bt_gateway.call(gid, "GET", "/api/status", timeout=_PROBE_TIMEOUT)
    if not ok or not isinstance(st, dict):
        return {"id": gid, "name": gid, "online": False, "ip": bt_gateway.last_seen(gid) or "", "error": err}
    bt, wifi = st.get("bt") or {}, st.get("wifi") or {}
    return {
        "id": gid, "name": st.get("name") or gid, "online": True, "ip": wifi.get("ip") or bt_gateway.last_seen(gid) or "",
        "mac": bt.get("mac", ""), "version": st.get("version", ""), "uptime_s": st.get("uptime_s", 0),
        "wifi_ssid": wifi.get("ssid", ""), "wifi_rssi": wifi.get("rssi", 0),
        "volume": bt.get("volume", -1), "bt_connected": bool(bt.get("connected")),
        "target_addr": (bt.get("target_addr") or "").lower(), "target_name": bt.get("target_name", ""),
    }


def list_gateways() -> List[Dict[str, Any]]:
    """Tutti i gateway noti (con un INI o visti di recente), con lo stato, interrogati in parallelo."""
    ids = sorted({g["device_id"] for g in bt_gateway.list_gateways()} | set(bt_gateway.seen_ids()))
    if not ids:
        return []
    with ThreadPoolExecutor(max_workers=min(8, len(ids))) as ex:
        return list(ex.map(probe, ids))


def info(gid: str) -> Dict[str, Any]:
    """Info del gateway + dispositivi conosciuti (con quale e' connesso / in uso)."""
    gw = probe(gid)
    # Una cassa gia' nota al gateway (es. scelta prima via USB) ma non nell'elenco: la si adotta.
    if gw["online"] and gw.get("target_addr"):
        try:
            bt_gateway.known_add(gid, gw["target_addr"], gw.get("target_name", ""))
        except bt_gateway.ValidationError:
            pass
    ini_spk = bt_gateway.speaker(gid)["addr"]
    known = []
    for d in bt_gateway.known_devices(gid):
        is_target = gw["online"] and gw.get("target_addr") == d["addr"]
        known.append({"addr": d["addr"], "name": d["name"],
                      "connected": bool(is_target and gw.get("bt_connected")),
                      "selected": bool(is_target or ini_spk == d["addr"])})
    return {"gateway": gw, "known": known}


def scan_start(gid: str) -> Tuple[bool, str]:
    ok, data, err = bt_gateway.call(gid, "POST", "/api/bt/scan", {"seconds": SCAN_SECONDS})
    if not ok:
        return False, err
    return bool((data or {}).get("ok", True)), (data or {}).get("error", "")


def scan_state(gid: str) -> Dict[str, Any]:
    """Scansione in corso? e dispositivi trovati (i piu' forti prima)."""
    ok, data, err = bt_gateway.call(gid, "GET", "/api/bt/devices")
    if not ok or not isinstance(data, dict):
        return {"ok": False, "error": err, "scanning": False, "found": []}
    known = {d["addr"] for d in bt_gateway.known_devices(gid)}
    found = []
    for d in data.get("devices", []):
        addr = (d.get("addr") or "").lower()
        if addr:
            found.append({"addr": addr, "name": d.get("name") or "", "rssi": d.get("rssi", 0), "known": addr in known})
    found.sort(key=lambda d: d["rssi"], reverse=True)
    return {"ok": True, "scanning": bool(data.get("scanning")), "found": found[:20]}


def _command(gid: str, path: str, body: Dict[str, Any]) -> Tuple[bool, str]:
    ok, data, err = bt_gateway.call(gid, "POST", path, body, timeout=3.0)
    if not ok:
        return False, err or "gateway non raggiungibile"
    if (data or {}).get("ok") is False:
        return False, (data or {}).get("error", "il gateway ha rifiutato il comando")
    return True, ""


def connect(gid: str, mac: str, name: str = "") -> Tuple[bool, str]:
    """Associa (aggiunge ai conosciuti) e connette: il gateway usera' questa cassa."""
    mac = bt_gateway.norm_mac(mac)
    ok, err = _command(gid, "/api/bt/connect", {"addr": mac, "name": name})
    if not ok:
        return False, err
    bt_gateway.known_add(gid, mac, name)
    bt_gateway.set_speaker(gid, mac, name)
    return True, ""


def disconnect(gid: str) -> Tuple[bool, str]:
    ok, err = _command(gid, "/api/bt/disconnect", {})
    if not ok:
        return False, err
    bt_gateway.set_speaker(gid, "")     # senza questo il giro di config successivo la riconnetterebbe
    return True, ""


def forget(gid: str, mac: str) -> Tuple[bool, str]:
    """Dissocia: toglie dall'elenco; se e' la cassa in uso, il gateway la dimentica e si disconnette."""
    mac = bt_gateway.norm_mac(mac)
    gw = probe(gid)
    in_use = bt_gateway.speaker(gid)["addr"] == mac or (gw["online"] and gw.get("target_addr") == mac)
    if in_use:
        ok, err = _command(gid, "/api/bt/forget", {})
        if not ok:
            return False, err
        bt_gateway.set_speaker(gid, "")
    bt_gateway.known_remove(gid, mac)
    return True, ""
