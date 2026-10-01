"""TV VIDAA con un broker MQTT vero (TLS, credenziali della TV) e una "TV finta"
che risponde all'abbinamento. Richiede mosquitto su 127.0.0.1:36669 (vedi
tests/README). Esecuzione: python3 tests/test_tv_vidaa.py"""
import json
import os
import ssl
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import paho.mqtt.client as mqtt  # noqa: E402
from app.core import tv_vidaa as tv  # noqa: E402

ok = fail = 0


def check(c, m):
    global ok, fail
    ok, fail = (ok + 1, fail) if c else (ok, fail + 1)
    print(("OK  " if c else "FAIL"), m)


tmp = tempfile.mkdtemp()
tv._DATA = tmp
tv._CONFIG = os.path.join(tmp, "tv.json")
tv._CERT_DIR = os.path.join(tmp, "tv")

# ---- TV finta: riceve i tasti, mostra "codice 4321", risponde all'abbinamento
received = []
fake = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="fake-tv")
fake.username_pw_set("hisenseservice", "multimqttservice")
fake.tls_set(cert_reqs=ssl.CERT_NONE)
fake.tls_insecure_set(True)


def on_msg(c, u, msg):
    received.append((msg.topic, msg.payload.decode()))
    parts = msg.topic.split("/")
    if msg.topic.endswith("/actions/authenticationcode"):
        cid = parts[4]
        ok_code = json.loads(msg.payload)["authNum"] == 4321
        c.publish(f"/remoteapp/mobile/{cid}/ui_service/data/authenticationcode", json.dumps({"result": 1 if ok_code else 0, "info": ""}))
    if msg.topic.endswith("/actions/gettvstate"):
        c.publish("/remoteapp/mobile/broadcast/ui_service/state", json.dumps({"statetype": "sourceswitch", "sourcename": "HDMI 1"}))


fake.on_message = on_msg
fake.connect("127.0.0.1", 36669)
fake.subscribe("/remoteapp/tv/#")
fake.loop_start()
time.sleep(0.5)

print("== Configurazione ==")
cfg = tv.load_config()
check(cfg["client_mac"].startswith("02:") and cfg["username"] == "hisenseservice", "identità generata (MAC locale), credenziali note")
try:
    tv.send_key("KEY_POWER")
    check(False, "senza IP: errore")
except RuntimeError as e:
    check("IP" in str(e), "senza IP: errore chiaro")
tv.update_config({"ip": "127.0.0.1", "mac": "dc:9a:7d:b5:ee:26", "protocol": "legacy"})

print("\n== Connessione e tasti ==")
tv.TV.ensure()
check(tv.TV.connected, "connessione TLS 1.2 sulla porta 36669 con le credenziali della TV")
tv.send_key("key_volumeup")
tv.send_key("KEY_OK")
time.sleep(0.5)
cid = tv.client_id(tv.load_config())
keys = [(t, p) for t, p in received if t.endswith("/sendkey")]
check(keys == [(f"/remoteapp/tv/remote_service/{cid}/actions/sendkey", "KEY_VOLUMEUP"),
               (f"/remoteapp/tv/remote_service/{cid}/actions/sendkey", "KEY_OK")], "tasti sull'argomento giusto: %s" % keys)
try:
    tv.send_key("rm -rf")
    check(False, "tasto sconosciuto rifiutato")
except ValueError:
    check(True, "tasto sconosciuto rifiutato")
tv.request_state()
time.sleep(0.5)
check(tv.status()["state"].get("sourcename") == "HDMI 1", "stato della TV ricevuto (sorgente)")

print("\n== Abbinamento ==")
tv.pair_start()
time.sleep(0.3)
check(any(t.endswith("/actions/vidaa_app_connect") for t, _ in received), "richiesta di abbinamento (la TV mostra il codice)")
check(tv.pair_confirm("1111", timeout=2) is False and not tv.load_config()["paired"], "codice sbagliato: non abbinata")
check(tv.pair_confirm("4321", timeout=2) is True and tv.load_config()["paired"], "codice giusto: abbinata e salvato")
tv.update_config({"ip": "127.0.0.2"})
check(not tv.load_config()["paired"], "cambiando IP (altra TV) l'abbinamento si azzera")

print("\n== TV spenta / credenziali ==")
tv.update_config({"ip": "127.0.0.1", "password": "sbagliata"})
try:
    tv.TV.ensure(timeout=3)
    check(False, "credenziali sbagliate rifiutate")
except RuntimeError as e:
    check("rifiutato" in str(e), "credenziali sbagliate: '%s'" % e)
tv.update_config({"ip": "127.0.0.9", "password": "multimqttservice"})
try:
    tv.TV.ensure(timeout=2)
    check(False, "TV spenta")
except RuntimeError as e:
    check("spenta" in str(e) or "risposta" in str(e), "TV spenta: '%s'" % e)

print("\n== Wake-on-LAN ==")
sent = []


class FakeSock:
    def __init__(self, *a): pass
    def __enter__(self): return self
    def __exit__(self, *a): pass
    def setsockopt(self, *a): pass
    def sendto(self, data, addr): sent.append((data, addr))


orig = tv.socket.socket
tv.socket.socket = FakeSock
tv.wake("DC:9A:7D:B5:EE:26", "192.168.1.85")
tv.socket.socket = orig
check(sent and sent[0][0] == bytes.fromhex("ff" * 6 + "dc9a7db5ee26" * 16), "pacchetto magico corretto (6×FF + 16× MAC)")
check(("192.168.1.255", 9) in [a for _, a in sent] and ("255.255.255.255", 9) in [a for _, a in sent], "broadcast della sottorete e generale")

print("\n== API ==")
from app.main import create_app  # noqa: E402

tv.update_config({"ip": "127.0.0.1", "protocol": "legacy"})
c = create_app().test_client()
r = c.post("/api/tv/key", json={"key": "KEY_MUTE"})
check(r.status_code == 200 and r.get_json()["status"]["connected"], "POST /api/tv/key")
check(c.post("/api/tv/key", json={"key": "BOH"}).status_code == 400, "tasto non valido: 400")
check("tv-remote" in c.get("/").get_data(as_text=True), "pannello telecomando nella pagina")

fake.loop_stop()

print("\n== Protocollo recente (vidaacommon, token) ==")
tv.PORT = 36671  # broker che accetta credenziali calcolate (la TV le verifica, qui no)
got = []
new = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="fake-tv-new")
new.tls_set(cert_reqs=ssl.CERT_NONE)
new.tls_insecure_set(True)


def on_new(c, u, msg):
    got.append(msg.topic)
    p = msg.topic.split("/")
    cid = p[4]
    if msg.topic.endswith("/actions/vidaa_app_connect"):
        v = json.loads(msg.payload)["app_version"]
        c.publish(f"/remoteapp/mobile/{cid}/ui_service/data/authentication", '""' if v >= 5 else '{"result":"app_too_old"}')
    elif msg.topic.endswith("/actions/authenticationcode"):
        good = json.loads(msg.payload)["authNum"] == 1234
        c.publish(f"/remoteapp/mobile/{cid}/ui_service/data/authenticationcode", json.dumps({"result": 1 if good else 0, "info": ""}))
    elif msg.topic.endswith("/data/gettoken"):
        rt = json.loads(msg.payload)["refreshtoken"]
        now = str(int(time.time()))
        c.publish(f"/remoteapp/mobile/{cid}/platform_service/data/tokenissuance", json.dumps({
            "accesstoken": "ACC2" if rt else "ACC1", "accesstoken_time": now, "accesstoken_duration_day": 2,
            "refreshtoken": "REF1", "refreshtoken_time": now, "refreshtoken_duration_day": 30}))


new.on_message = on_new
new.connect("127.0.0.1", 36671)
new.subscribe("/remoteapp/tv/#")
new.loop_start()
time.sleep(0.5)
tv.update_config({"ip": "127.0.0.1", "protocol": "vidaa"})
cfg = tv.load_config()
cid = tv.client_id(cfg)
check(cid.endswith("_vidaacommon_001") and "$his$" in cid, "identità del protocollo recente: %s" % cid)
u, pw = tv.generated_credentials(1790500000)
check(u == "his$1790500000" and len(pw) == 32 and pw.isupper(), "credenziali calcolate dall'ora (his$<ora>, MD5)")
res = tv.pair_start()
check(res["app_version"] == 5 and [p["app_version"] for p in res["prove"]] == [2, 3, 4, 5],
      "versione dell'app: provate 2,3,4 (rifiutate), accettata 5 e ricordata")
check(tv.load_config()["app_version"] == 5 and tv.pair_start()["prove"][0]["app_version"] == 5,
      "alla volta successiva si parte da quella accettata")
check(any(t.endswith(f"{cid}/actions/vidaa_app_connect") for t in got), "richiesta del codice con l'identità nuova")
check(tv.pair_confirm("9999", timeout=2) is False, "codice sbagliato: niente token")
check(tv.pair_confirm("1234", timeout=3) is True, "codice giusto: abbinata")
cfg = tv.load_config()
check(cfg["paired"] and cfg["tokens"]["accesstoken"] == "ACC1" and cfg["tokens"]["username"].startswith("his$"),
      "token di accesso e di rinnovo salvati, con l'utente dell'abbinamento")
tv.send_key("KEY_VOLUMEUP")
check(tv.TV.connected and not tv.TV.pairing_session, "dopo l'abbinamento ci si collega con il token")
cfg["tokens"]["accesstoken_time"] = str(int(time.time()) - 5 * 86400)  # token scaduto
tv.save_config(cfg)
tv.TV.reset()
tv.send_key("KEY_OK")
check(tv.load_config()["tokens"]["accesstoken"] == "ACC2", "token scaduto: rinnovato da solo con il token di rinnovo")
cfg = tv.load_config()
cfg["tokens"]["refreshtoken_time"] = "1000"
cfg["tokens"]["accesstoken_time"] = "1000"
tv.save_config(cfg)
tv.TV.reset()
try:
    tv.send_key("KEY_OK")
    check(False, "abbinamento scaduto")
except RuntimeError as e:
    check("Abbina" in str(e) and not tv.load_config()["paired"], "anche il rinnovo scaduto: chiede di riabbinare")
new.loop_stop()

print("\n%d ok, %d falliti" % (ok, fail))
sys.exit(1 if fail else 0)
