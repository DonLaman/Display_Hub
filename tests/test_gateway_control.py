"""Pilotaggio dei gateway dal display: elenco, info, scansione, connetti/disconnetti/dissocia, con un
finto gateway HTTP (stessa API del firmware) e il login attivo. Esecuzione: python3 tests/test_gateway_control.py"""
import json
import os
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = os.path.join(os.path.dirname(__file__), "..")
sys.path.insert(0, ROOT)
os.environ["DH_AUTH_PASSWORD"] = "segreta"
from app.core import bt_gateway, gateway_control  # noqa: E402
from app.main import create_app                    # noqa: E402

ok = fail = 0


def check(c, m):
    global ok, fail
    ok, fail = (ok + 1, fail) if c else (ok, fail + 1)
    print(("OK  " if c else "FAIL"), m)


class FakeGateway:
    """Come il firmware: scansione asincrona (scanning + elenco), una sola cassa (target)."""
    def __init__(self):
        self.target_addr, self.target_name, self.connected, self.scanning = "aa:bb:cc:dd:ee:01", "Cassa Salotto", True, False
        self.devices = [{"name": "Cassa Salotto", "addr": "aa:bb:cc:dd:ee:01", "rssi": -50},
                        {"name": "", "addr": "aa:bb:cc:dd:ee:02", "rssi": -80},
                        {"name": "TV", "addr": "aa:bb:cc:dd:ee:03", "rssi": -60}]
        self.calls, self.reject_connect, self.token = [], False, False


gw = FakeGateway()


class H(BaseHTTPRequestHandler):
    def _send(self, code, obj):
        raw = json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Length", str(len(raw))); self.end_headers(); self.wfile.write(raw)

    def do_GET(self):
        if gw.token:
            return self._send(401, {"error": "token"})
        if self.path == "/api/status":
            self._send(200, {"type": "gateway_status", "name": "Salotto", "version": "1.0.0", "uptime_s": 321,
                             "wifi": {"connected": True, "ssid": "CasaWiFi", "ip": "127.0.0.1", "rssi": -55},
                             "bt": {"connected": gw.connected, "target_addr": gw.target_addr, "target_name": gw.target_name,
                                    "scanning": gw.scanning, "volume": 60, "mac": "24:6F:28:AA:BB:CC"}})
        elif self.path == "/api/bt/devices":
            self._send(200, {"type": "gateway_devices", "scanning": gw.scanning, "devices": gw.devices})
        else:
            self._send(404, {})

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        gw.calls.append((self.path, body))
        if gw.token:
            return self._send(401, {"error": "token"})
        if self.path == "/api/bt/scan":
            gw.scanning = True
        elif self.path == "/api/bt/connect":
            if gw.reject_connect:
                return self._send(200, {"type": "gateway_result", "cmd": "bt_connect", "ok": False, "error": "indirizzo non valido"})
            gw.target_addr, gw.target_name, gw.connected = body["addr"], body.get("name", ""), True
        elif self.path in ("/api/bt/disconnect", "/api/bt/forget"):
            gw.target_addr, gw.target_name, gw.connected = "", "", False
        self._send(200, {"type": "gateway_result", "ok": True})

    def log_message(self, *a):
        pass


srv = HTTPServer(("127.0.0.1", 0), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
bt_gateway._DIR = tempfile.mkdtemp()
bt_gateway._PUSH_PORT = srv.server_port
bt_gateway.update_from_dict("g1", {"volume": 60})
bt_gateway.update_from_dict("g2", {"volume": 50})            # ha un INI ma non e' mai stato visto
bt_gateway.update_from_dict("g3", {"volume": 50})
bt_gateway.note_seen("g1", "127.0.0.1")
bt_gateway.note_seen("g3", "127.0.0.2")                      # visto, ma ora non risponde
A1, A2, A3 = "aa:bb:cc:dd:ee:01", "aa:bb:cc:dd:ee:02", "aa:bb:cc:dd:ee:03"

print("== Elenco dei gateway ==")
lst = gateway_control.list_gateways()
by = {g["id"]: g for g in lst}
check([g["id"] for g in lst] == ["g1", "g2", "g3"], "tutti i gateway noti, in ordine: %s" % [g["id"] for g in lst])
g1 = by["g1"]
check(g1["online"] and g1["name"] == "Salotto" and g1["mac"] == "24:6F:28:AA:BB:CC" and g1["version"] == "1.0.0" and g1["ip"] == "127.0.0.1", "gateway raggiungibile: nome, MAC, versione e IP dallo stato del gateway")
check(not by["g2"]["online"] and by["g2"]["name"] == "g2", "mai visto: offline, il nome e' l'id")
check(not by["g3"]["online"] and by["g3"]["error"], "visto ma spento: offline con il motivo (%s)" % by["g3"].get("error", "")[:40])

print("\n== Info e dispositivi conosciuti ==")
i = gateway_control.info("g1")
check(i["gateway"]["wifi_ssid"] == "CasaWiFi" and i["gateway"]["bt_connected"], "info: WiFi e stato Bluetooth")
check([k["addr"] for k in i["known"]] == [A1] and i["known"][0]["name"] == "Cassa Salotto", "la cassa gia' in uso sul gateway viene adottata nell'elenco dei conosciuti")
check(i["known"][0]["connected"] and i["known"][0]["selected"], "...e risulta connessa e in uso")
check(bt_gateway.known_devices("g1") == [{"addr": A1, "name": "Cassa Salotto"}], "l'elenco e' salvato nell'INI del gateway")
check(len(gateway_control.info("g2")["known"]) == 0 and gateway_control.info("g2")["gateway"]["online"] is False, "gateway spento: info comunque disponibile (elenco dall'INI)")

print("\n== Scansione ==")
ok_, err = gateway_control.scan_start("g1")
check(ok_ and gw.scanning and gw.calls[-1] == ("/api/bt/scan", {"seconds": gateway_control.SCAN_SECONDS}), "scan_start: il gateway parte (durata %d s)" % gateway_control.SCAN_SECONDS)
st = gateway_control.scan_state("g1")
check(st["ok"] and st["scanning"] and [d["addr"] for d in st["found"]] == [A1, A3, A2], "scan_state: in corso, trovati ordinati per segnale")
check(st["found"][0]["known"] is True and st["found"][1]["known"] is False, "i trovati dicono se sono gia' conosciuti")
check(gateway_control.scan_start("g2")[0] is False, "scansione su un gateway spento: errore")

print("\n== Connetti, disconnetti, dissocia ==")
ok_, err = gateway_control.connect("g1", A3.upper(), "TV")
check(ok_ and gw.target_addr == A3 and bt_gateway.speaker("g1")["addr"] == A3, "connect: il gateway cambia cassa e l'INI la segna in uso")
check({d["addr"] for d in bt_gateway.known_devices("g1")} == {A1, A3}, "connect: la nuova cassa entra tra i conosciuti")
k = {d["addr"]: d for d in gateway_control.info("g1")["known"]}
check(k[A3]["connected"] and not k[A1]["connected"], "info: connessa la TV, non piu' la cassa")
gw.reject_connect = True
before = (bt_gateway.speaker("g1"), bt_gateway.known_devices("g1"))
ok_, err = gateway_control.connect("g1", A2, "Nuova")
check(not ok_ and "non valido" in err and before == (bt_gateway.speaker("g1"), bt_gateway.known_devices("g1")), "connect rifiutato dal gateway: errore e INI invariato")
gw.reject_connect = False
try:
    gateway_control.connect("g1", "xyz"); bad = False
except bt_gateway.ValidationError:
    bad = True
check(bad, "indirizzo non valido rifiutato")
ok_, err = gateway_control.disconnect("g1")
check(ok_ and gw.target_addr == "" and bt_gateway.speaker("g1")["addr"] == "" and len(bt_gateway.known_devices("g1")) == 2, "disconnect: gateway scollegato, nessuna cassa in uso nell'INI, conosciuti intatti")
gateway_control.connect("g1", A1, "Cassa Salotto")
bt_gateway.known_add("g1", A2, "Vecchia")
n_calls = len(gw.calls)
ok_, err = gateway_control.forget("g1", A2)
check(ok_ and len(gw.calls) == n_calls and A2 not in {d["addr"] for d in bt_gateway.known_devices("g1")}, "dissocia una cassa NON in uso: tolta dall'elenco senza toccare il gateway")
ok_, err = gateway_control.forget("g1", A1)
check(ok_ and gw.calls[-1][0] == "/api/bt/forget" and gw.target_addr == "" and bt_gateway.speaker("g1")["addr"] == "" and A1 not in {d["addr"] for d in bt_gateway.known_devices("g1")},
      "dissocia la cassa in uso: il gateway la dimentica, nessuna in uso, tolta dall'elenco")
bt_gateway.known_add("g3", A1, "Cassa"); bt_gateway.set_speaker("g3", A1, "Cassa")
ok_, err = gateway_control.forget("g3", A1)
check(not ok_ and bt_gateway.speaker("g3")["addr"] == A1 and len(bt_gateway.known_devices("g3")) == 1, "dissocia la cassa in uso con il gateway spento: errore, INI invariato")
bt_gateway.known_add("g3", A2, "Altra")
check(gateway_control.forget("g3", A2)[0] and [d["addr"] for d in bt_gateway.known_devices("g3")] == [A1], "...ma una cassa non in uso si toglie anche con il gateway spento")

print("\n== Gateway con token ==")
gw.token = True
p = gateway_control.probe("g1")
check(not p["online"] and "token" in p["error"], "gateway con token: offline, e l'errore lo dice")
ok_, err = gateway_control.connect("g1", A3, "TV")
check(not ok_ and "token" in err, "connect: errore chiaro (%s)" % err)
gw.token = False

print("\n== Rotte (login attivo, senza cookie) ==")
c = create_app().test_client()
bt_gateway.note_seen("g1", "127.0.0.1")
r = c.get("/api/esp/bt-gateways")
check(r.status_code == 200 and r.get_json()["ok"] and {g["id"] for g in r.get_json()["gateways"]} == {"g1", "g2", "g3"}, "GET /api/esp/bt-gateways: 200 senza login")
r = c.get("/api/esp/bt-gateways/g1")
check(r.status_code == 200 and r.get_json()["gateway"]["name"] == "Salotto" and "known" in r.get_json(), "GET /api/esp/bt-gateways/g1: info e conosciuti")
check(c.post("/api/esp/bt-gateways/g1/scan").status_code == 200 and c.get("/api/esp/bt-gateways/g1/scan").get_json()["found"], "scan: POST avvia, GET legge")
r = c.post("/api/esp/bt-gateways/g1/connect", json={"addr": A3, "name": "TV"})
check(r.status_code == 200 and r.get_json()["ok"] and gw.target_addr == A3, "POST connect")
check(c.post("/api/esp/bt-gateways/g1/connect", json={"addr": "zz"}).status_code == 400, "connect con indirizzo non valido: 400")
check(c.post("/api/esp/bt-gateways/g1/disconnect").status_code == 200 and gw.target_addr == "", "POST disconnect")
c.post("/api/esp/bt-gateways/g1/connect", json={"addr": A1, "name": "Cassa Salotto"})
check(c.post("/api/esp/bt-gateways/g1/forget", json={"addr": A1}).status_code == 200 and bt_gateway.speaker("g1")["addr"] == "", "POST forget")
check(c.post("/api/esp/bt-gateways/g2/connect", json={"addr": A1}).status_code == 502, "gateway spento: 502 con l'errore")
check(c.get("/api/esp/bt-gateways/..%2Fx").status_code in (400, 404) and c.get("/api/esp/bt-gateways/a b").status_code in (400, 404), "id non valido: rifiutato")
check(c.get("/api/esp/bt-gateways", headers={"X-Forwarded-For": "8.8.8.8"}).status_code == 403, "dal reverse proxy: 403")
check(c.get("/api/bt-gateway/g1").status_code == 401, "la config del gateway dalla web UI resta dietro il login")

print("\n%d ok, %d falliti" % (ok, fail))
sys.exit(1 if fail else 0)
