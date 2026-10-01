"""Test della sincronizzazione configurazione display <-> server.
Esecuzione:  python3 tests/test_device_config.py   (dalla radice del progetto)"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from app.core import device_config as dc  # noqa: E402
from app.core.ini_doc import IniDoc  # noqa: E402

ok = fail = 0


def check(cond, msg):
    global ok, fail
    if cond:
        ok += 1
        print("OK  ", msg)
    else:
        fail += 1
        print("FAIL", msg)


print("== File INI (stesse regole del display) ==")
src = "# Display Hub\r\n[Server]\r\nHost = 192.168.1.40   \r\n; nota\r\n\r\n[wifi]\r\nnetwork = Casa | p#ss;word\r\nnetwork = Hot | \" sp \"\r\nstrana\r\n"
d = IniDoc(src)
check(d.get("server", "host") == "192.168.1.40", "sezioni/chiavi senza maiuscole, spazi tolti")
check(d.get_all("wifi", "network") == ["Casa | p#ss;word", "Hot |  sp "] or d.get_all("wifi", "network")[0] == "Casa | p#ss;word",
      "'#' e ';' nel valore restano")
out = d.serialize()
check("# Display Hub" in out and "; nota" in out and "strana" in out, "commenti e righe sconosciute conservati")
d.set("server", "host", "10.0.0.5")
d.set("server", "port", "12000")
d.set("vpn", "enabled", "1")
r = IniDoc(d.serialize())
check(r.get("server", "host") == "10.0.0.5" and r.get("server", "port") == "12000" and r.get("vpn", "enabled") == "1",
      "modifica, aggiunta, sezione nuova")
check("Host = 10.0.0.5" in r.serialize(), "forma della chiave scritta a mano conservata")
d.set("x", "p", ' a"b ')
check(IniDoc(d.serialize()).get("x", "p") == ' a"b ', "virgolette: andata e ritorno")

print("\n== Unione a tre vie ==")
base = "[server]\nhost = 192.168.1.40\nport = 12000\n[wifi]\nnetwork = Casa | a\nnetwork = Hot | b\n"
server = "[server]\nhost = 192.168.1.50\nport = 12000\n[wifi]\nnetwork = Casa | a\nnetwork = Hot | b\nnetwork = Uff | c\n"
device = "# mio\n[server]\nhost = 192.168.1.40\nport = 13000\n[wifi]\nnetwork = Hot | b2\nnetwork = Casa | a\n[bt]\ndevice = aa:bb | 0 | S\n"
m = IniDoc(dc.merge_three_way(base, server, device))
check(m.get("server", "host") == "192.168.1.50", "voce cambiata solo sul server: vince il server")
check(m.get("server", "port") == "13000", "voce cambiata dal display: vince il display")
nets = m.get_all("wifi", "network")
check(nets[:2] == ["Hot | b2", "Casa | a"] and "Uff | c" in nets, "reti: ordine e password dal display, aggiunta del server in coda")
check(m.get_all("bt", "device") == ["aa:bb | 0 | S"], "dispositivo BT aggiunto dal display")
check("# mio" in m.serialize(), "commenti del display conservati")
m2 = IniDoc(dc.merge_three_way(base, server, "[server]\nhost = 192.168.1.40\nport = 12000\n[wifi]\nnetwork = Casa | a\n"))
check(m2.get_all("wifi", "network") == ["Casa | a", "Uff | c"], "rete tolta dal display resta tolta; aggiunta del server resta")
m3 = IniDoc(dc.merge_three_way(None, server, "[server]\nport = 14000\nhost_vpn = 100.1.1.1\n[wifi]\nnetwork = Treno | t\n"))
check(m3.get("server", "host") == "192.168.1.50" and m3.get("server", "port") == "12000",
      "senza base (es. microSD nuova con i valori della build): sui valori vince il server")
check(m3.get("server", "host_vpn") == "100.1.1.1", "senza base: valore presente solo sul display tenuto")
check([n.split(" |")[0] for n in m3.get_all("wifi", "network")] == ["Casa", "Hot", "Uff", "Treno"],
      "senza base: reti unite, prima quelle del server, nessuna persa")

print("\n== Archivio e sincronizzazione ==")
with tempfile.TemporaryDirectory() as tmp:
    dc._DIR = tmp
    r = dc.sync_from_device("sala-01", 0, None)
    check(r == {"rev": 0, "text": None}, "display nuovo, niente da scaricare")
    r = dc.sync_from_device("sala-01", 0, "[server]\nhost = 1.1.1.1\n[sync]\nrev = 0\n")
    check(r["rev"] == 1 and "[sync]" not in r["text"], "primo invio: rev 1, sezione [sync] del display esclusa")
    r2 = dc.sync_from_device("sala-01", 1, "[server]\nhost = 1.1.1.1\n")
    check(r2["rev"] == 1, "invio identico: nessuna nuova revisione")
    w = dc.save_from_web("sala-01", 1, "[server]\nhost = 2.2.2.2\nport = 9\n")
    check(w["rev"] == 2, "modifica dalla web UI: rev 2")
    try:
        dc.save_from_web("sala-01", 1, "[server]\nhost = 3.3.3.3\n")
        check(False, "web UI su versione vecchia rifiutata")
    except dc.ConflictError as e:
        check(e.current["rev"] == 2, "web UI su versione vecchia rifiutata (conflitto con la versione attuale)")
    check(dc.sync_from_device("sala-01", 1, None)["text"].find("2.2.2.2") >= 0, "il display scarica la modifica della web UI")
    r3 = dc.sync_from_device("sala-01", 1, "[server]\nhost = 1.1.1.1\n[wifi]\nnetwork = N | x\n")
    t3 = IniDoc(r3["text"])
    check(r3["rev"] == 3 and t3.get("server", "host") == "2.2.2.2" and t3.get("server", "port") == "9"
          and t3.get_all("wifi", "network") == ["N | x"],
          "modifiche contemporanee: web UI (host/porta) + display (rete) unite in rev 3")
    check(dc.get("sala-01")["source"] == "device" and dc.current_rev("sala-01") == 3, "stato e revisione")
    for i in range(40):
        dc.save_from_web("sala-01", dc.current_rev("sala-01"), "[server]\nport = %d\n" % i)
    rec = dc._load("sala-01")
    check(len(rec["history"]) == dc.HISTORY_KEEP, "storico limitato alle ultime %d versioni" % dc.HISTORY_KEEP)
    r4 = dc.sync_from_device("sala-01", 2, "[server]\nport = 777\n")
    check(IniDoc(r4["text"]).get("server", "port") == "39", "base troppo vecchia (fuori storico): trattata come sconosciuta, vince il server")
    check(dc.valid_device_id("sala-01") and not dc.valid_device_id("../x") and not dc.valid_device_id(""), "identificativi validi")

print("\n%d ok, %d falliti" % (ok, fail))
sys.exit(1 if fail else 0)
