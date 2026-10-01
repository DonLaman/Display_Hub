"""Scenari di sincronizzazione display <-> server: un display simulato (stesse
regole di cfg_sync_init/cfg_sync in firmware/src/main.cpp) parla con la VERA
web app Flask (client di test). Esecuzione: python3 tests/test_sync_protocol.py"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from app.core import device_config as dc  # noqa: E402
from app.core.ini_doc import IniDoc  # noqa: E402
from app.main import create_app  # noqa: E402

ok = fail = 0


def check(cond, msg):
    global ok, fail
    ok, fail = (ok + 1, fail) if cond else (ok, fail + 1)
    print(("OK  " if cond else "FAIL"), msg)


class SimDisplay:
    """Imitazione del firmware: config = testo INI; persistent = microSD/NVS."""

    def __init__(self, client, device_id, text, persistent):
        self.c, self.id, self.text, self.persistent = client, device_id, text, persistent
        self.synced, self.known = None, False
        # cfg_sync_init()
        if not persistent:
            self.set_rev(0)
            self.synced, self.known = self.sync_text(), True

    def doc(self):
        return IniDoc(self.text)

    def set_rev(self, rev):
        d = self.doc()
        d.set("sync", "rev", str(rev))
        self.text = d.serialize()

    def sync_text(self):
        d = self.doc()
        d.remove("sync", "rev")
        return d.serialize()

    def edit(self, section, key, value):
        d = self.doc()
        d.set(section, key, value)
        self.text = d.serialize()

    def poll(self):
        server_rev = self.c.get("/api/esp/poll?device_id=" + self.id).get_json()["cfg_rev"]
        # cfg_sync()
        cur = self.sync_text()
        local_changed = not self.known or cur != self.synced
        base_rev = int(self.doc().get("sync", "rev", "0") or 0)
        if not local_changed and server_rev == base_rev and server_rev != 0:
            return "allineato"
        send = local_changed or server_rev == 0
        r = self.c.post("/api/esp/config", json={"device_id": self.id, "base_rev": base_rev,
                                                  "text": cur if send else None}).get_json()
        if r["text"] is not None and r["text"] != cur:
            self.text = r["text"]  # DhConfig::replaceAll
        self.set_rev(r["rev"])
        self.synced, self.known = self.sync_text(), True
        return "sincronizzato"

    def get(self, s, k):
        return self.doc().get(s, k)


BUILD = "[server]\nhost = 192.168.1.40\nport = 12000\n[wifi]\nnetwork = Casa | pw\n"

with tempfile.TemporaryDirectory() as tmp:
    dc._DIR = tmp
    dc._rev_cache.clear()
    c = create_app().test_client()

    print("== Primo avvio, senza microSD ==")
    a = SimDisplay(c, "sala-01", BUILD, persistent=False)
    a.poll()
    check(dc.current_rev("sala-01") == 1, "il server riceve la configurazione (copia di riserva)")
    check(a.poll() == "allineato", "poll successivo: niente da fare")

    print("\n== Modifica sul display (nuova rete) ==")
    d = a.doc(); d.set_all("wifi", "network", ["Hotspot | h", "Casa | pw"]); a.text = d.serialize()
    a.poll()
    check("Hotspot" in dc.get("sala-01")["text"], "la rete aggiunta sul display arriva al server")

    print("\n== Modifica dalla web UI ==")
    cur = c.get("/api/devices/sala-01/config").get_json()
    t = IniDoc(cur["text"]); t.set("server", "port", "13000")
    check(c.put("/api/devices/sala-01/config", json={"base_rev": cur["rev"], "text": t.serialize()}).status_code == 200,
          "salvata dalla web UI")
    a.poll()
    check(a.get("server", "port") == "13000", "il display la riceve al poll successivo")

    print("\n== Riavvio senza microSD: si riparte dalla copia del server ==")
    b = SimDisplay(c, "sala-01", BUILD, persistent=False)
    b.poll()
    check(b.get("server", "port") == "13000" and "Hotspot" in b.text,
          "i valori della build NON sovrascrivono il server: il display riprende rete e porta")

    print("\n== Modifiche contemporanee (display offline + web UI) ==")
    b.edit("server", "host_vpn", "100.1.2.3")          # sul display, senza rete
    cur = c.get("/api/devices/sala-01/config").get_json()
    t = IniDoc(cur["text"]); t.set("server", "host", "192.168.1.41")
    c.put("/api/devices/sala-01/config", json={"base_rev": cur["rev"], "text": t.serialize()})
    b.poll()
    check(b.get("server", "host") == "192.168.1.41" and b.get("server", "host_vpn") == "100.1.2.3",
          "unione: modifica della web UI + modifica del display, nessuna persa")
    check(b.poll() == "allineato", "poi allineati")

    print("\n== microSD nuova (creata con i valori della build, mai sincronizzata) ==")
    s = SimDisplay(c, "sala-01", BUILD, persistent=True)
    s.poll()
    check(s.get("server", "host") == "192.168.1.41" and s.get("server", "port") == "13000" and "Hotspot" in s.text,
          "vince la copia del server, non i valori della build")
    check(s.doc().get("sync", "rev") == str(dc.current_rev("sala-01")), "[sync] rev annotata nel file")

    print("\n== Riavvio con microSD già sincronizzata e modificata dal PC ==")
    s2 = SimDisplay(c, "sala-01", s.text, persistent=True)
    s2.edit("server", "port", "14000")  # modifica fatta a mano sul PC
    s2.poll()
    check(dc.get("sala-01")["text"].find("port = 14000") >= 0, "la modifica fatta sul PC arriva al server (base nota)")

print("\n%d ok, %d falliti" % (ok, fail))
sys.exit(1 if fail else 0)
