"""Player Bluetooth: widget, azioni (volume vero, fonti segnaposto), rotte con il login attivo,
invio immediato del volume al gateway e coerenza widget/firmware (icone, stili).
Esecuzione: python3 tests/test_bt_player.py"""
import json
import os
import re
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = os.path.join(os.path.dirname(__file__), "..")
sys.path.insert(0, ROOT)
os.environ["DH_AUTH_PASSWORD"] = "segreta"          # login attivo, come in produzione
from app.core import audio_player, bt_gateway       # noqa: E402
from app.main import create_app                     # noqa: E402
from app.widgets.bt_player import BtPlayerWidget    # noqa: E402

ok = fail = 0


def check(c, m):
    global ok, fail
    ok, fail = (ok + 1, fail) if c else (ok, fail + 1)
    print(("OK  " if c else "FAIL"), m)


bt_gateway._DIR = tempfile.mkdtemp()
fw = open(os.path.join(ROOT, "firmware", "src", "main.cpp"), encoding="utf-8").read()
icons_h = open(os.path.join(ROOT, "firmware", "lib", "dh_icons", "src", "dh_icons.h")).read()
fn = fw[fw.index("static lv_color_t btn_style_color"):]
fn = fn[:fn.index("\n}\n")]
fw_styles = set(re.findall(r'style == "(\w+)"', fn))
ifn = fw[fw.index("static const lv_img_dsc_t* icon_by_name"):]
ifn = ifn[:ifn.index("\n}\n")]
fw_icons = set(re.findall(r'name == "(\w+)"', ifn))
h_icons = set(re.findall(r"dhicon_(\w+);", icons_h))

print("== Widget ==")
w = BtPlayerWidget()
d = w.get_data()
main, srcs = d["views"]["main"], d["views"]["sources"]
check(w.meta.layout_type == "button_grid" and set(d["views"]) == {"main", "sources"} and d["initial_view"] == "main", "button_grid con due viste: main e sources, parte da main")
h = main.get("header") or {}
check(h.get("height") == audio_player.HEADER_H and h.get("title") and "progress" in h, "la vista main ha la scheda 'in riproduzione' (altezza %s)" % h.get("height"))
check("header" not in srcs, "la pagina delle fonti non ha la scheda")
check(any(b["action"] == "view:sources" for b in main["buttons"]), "un pulsante di main porta alla pagina delle fonti")
check(any(b["action"] == "view:main" for b in srcs["buttons"]), "la pagina delle fonti ha il ritorno al player")
src_btn = [b for b in srcs["buttons"] if b["action"].startswith("source:")]
check({b["action"].split(";")[0].split(":")[1] for b in src_btn} == set(audio_player.SOURCES), "le tre fonti MP3 / YouTube / Web radio hanno un pulsante ciascuna")
check(all(b["action"].endswith(";view:main") for b in src_btn) and all("in arrivo" in b["label"] for b in src_btn), "scegliere una fonte torna al player; le etichette dicono 'in arrivo'")
for name, view in (("main", main), ("sources", srcs)):
    cols, cells, bad = view["columns"], set(), 0
    for b in view["buttons"]:
        span = b.get("colspan", 1)
        bad += b["col"] < 0 or b["col"] + span > cols
        for c in range(b["col"], b["col"] + span):
            bad += (b["row"], c) in cells
            cells.add((b["row"], c))
    check(bad == 0, "vista %s (%d colonne): nessun bottone fuori griglia o sovrapposto" % (name, cols))
allb = main["buttons"] + srcs["buttons"]
used_icons = {b["icon"] for b in allb if b.get("icon")}
used_styles = {b["style"] for b in allb if b.get("style")}
check(used_icons <= fw_icons and fw_icons <= h_icons, "icone: tutte mappate nel firmware e presenti in dh_icons.h (mancanti: %s)" % (sorted(used_icons - fw_icons) or "nessuna"))
check(used_styles <= fw_styles, "stili: tutti con un colore nel firmware (mancanti: %s)" % (sorted(used_styles - fw_styles) or "nessuno"))
check(d["action_endpoint"] == "/api/esp/audio-action" and BtPlayerWidget().get_data({"gateway_id": "salotto"})["action_endpoint"].endswith("?gateway=salotto"), "endpoint di /api/esp (senza login); con gateway_id aggiunge ?gateway=")

print("\n== Azioni ==")
g = "t-azioni"
bt_gateway.update_from_dict(g, {"volume": 50})
check(audio_player.handle_action("vol:up", g)["volume"] == 55 and bt_gateway.to_dict(g)["volume"] == 55, "vol:up: +5 e memorizzato nell'INI")
check(audio_player.handle_action("vol:down", g)["volume"] == 50, "vol:down: -5")
bt_gateway.update_from_dict(g, {"volume": 98}); audio_player.handle_action("vol:up", g)
check(bt_gateway.to_dict(g)["volume"] == 100, "il volume non supera 100")
bt_gateway.update_from_dict(g, {"volume": 2}); audio_player.handle_action("vol:down", g)
check(bt_gateway.to_dict(g)["volume"] == 0, "il volume non scende sotto 0")
bt_gateway.update_from_dict(g, {"volume": 60})
audio_player.handle_action("vol:mute", g)
muted = bt_gateway.to_dict(g)["volume"]
check(muted == 0 and audio_player.header(g)["badge"] == "MUTO", "mute: volume 0 e la scheda dice MUTO")
audio_player.handle_action("vol:mute", g)
check(bt_gateway.to_dict(g)["volume"] == 60 and "60" in audio_player.header(g)["badge"], "secondo mute: ripristina il 60")
check(audio_player.header(g)["title"] == "Nessuna riproduzione", "senza fonte: 'Nessuna riproduzione'")
r = audio_player.handle_action("source:youtube;view:main", g)
hd = audio_player.header(g)
check(r.get("source") == "youtube" and hd["badge"].startswith("YOUTUBE") and "non ancora disponibile" in hd["subtitle"], "fonte YouTube scelta (azione composta, la parte view: si ignora): la scheda lo mostra, segnaposto")
for a in ("audio:play_pause", "audio:next", "audio:prev", "audio:stop"):
    try:
        audio_player.handle_action(a, g); got = False
    except audio_player.NotAvailable:
        got = True
    check(got, "%s: 'non ancora disponibile' (nessuna finta riproduzione)" % a)
for a in ("", "boh:x", "source:tv", "vol:sideways", "audio:fly"):
    try:
        audio_player.handle_action(a, g); got = False
    except ValueError:
        got = True
    check(got, "azione non valida rifiutata: %r" % a)
check(audio_player.resolve_gateway(None) in [x["device_id"] for x in bt_gateway.list_gateways()] and audio_player.resolve_gateway("") != "", "gateway predefinito: il primo configurato")

print("\n== Rotte con il login attivo ==")
app = create_app()
c = app.test_client()
r = c.post("/api/esp/audio-action?gateway=t-rotte", json={"action": "vol:up"})
check(r.status_code == 200 and r.get_json()["ok"] and r.get_json()["volume"] == 65, "POST /api/esp/audio-action senza login: 200, volume 65 (non 401)")
r = c.post("/api/esp/audio-action?gateway=t-rotte", json={"action": "audio:play_pause"})
check(r.status_code == 501 and r.get_json()["placeholder"], "play/pausa: 501 'placeholder'")
check(c.post("/api/esp/audio-action", json={"action": "boh"}).status_code == 400, "azione non valida: 400")
check(c.post("/api/esp/audio-action", data="non json", headers={"Content-Type": "text/plain"}).status_code == 400, "corpo non JSON: 400, non un crash")
r = c.get("/api/esp/bt-gateway-config?device_id=t-rotte")
check(r.status_code == 200 and "[bt]" in r.get_data(as_text=True) and "volume = 65" in r.get_data(as_text=True), "GET /api/esp/bt-gateway-config senza login: 200 con l'INI (il gateway non riceve piu' 401)")
check(c.get("/api/bt-gateway/config?device_id=t-rotte").status_code == 401, "la vecchia rotta /api/bt-gateway/config resta protetta dal login (401)")
check(c.get("/api/esp/bt-gateway-config?device_id=../x").status_code == 400, "device_id non valido: 400")
check(c.post("/api/esp/audio-action", json={"action": "vol:up"}, headers={"X-Forwarded-For": "8.8.8.8"}).status_code == 403, "dal reverse proxy: 403 (come le altre /api/esp)")
check(bt_gateway.last_seen("t-rotte") == "127.0.0.1", "il server ha annotato da dove il gateway scarica la config")

print("\n== Invio immediato del volume al gateway ==")
got_req = []


class Fake(BaseHTTPRequestHandler):
    def do_POST(self):
        got_req.append((self.path, json.loads(self.rfile.read(int(self.headers["Content-Length"])))))
        self.send_response(200); self.end_headers(); self.wfile.write(b"{}")

    def log_message(self, *a):
        pass


srv = HTTPServer(("127.0.0.1", 0), Fake)
threading.Thread(target=srv.serve_forever, daemon=True).start()
bt_gateway._PUSH_PORT = srv.server_port
bt_gateway.update_from_dict("t-push", {"volume": 40})
r = c.post("/api/esp/audio-action?gateway=t-push", json={"action": "vol:up"})
check(r.get_json()["pushed"] is False and got_req == [], "gateway mai visto: nessun invio, ma il volume e' nell'INI (lo applichera' al prossimo giro)")
c.get("/api/esp/bt-gateway-config?device_id=t-push")
r = c.post("/api/esp/audio-action?gateway=t-push", json={"action": "vol:up"})
check(r.get_json()["pushed"] is True and got_req == [("/api/audio/volume", {"volume": 50})], "gateway visto: POST /api/audio/volume {volume: 50} arrivato subito")
srv.shutdown()
r = c.post("/api/esp/audio-action?gateway=t-push", json={"action": "vol:up"})
j = r.get_json()
check(r.status_code == 200 and j["pushed"] is False and j["push_error"] and bt_gateway.to_dict("t-push")["volume"] == 55, "gateway spento: nessun errore per il display, volume salvato (55)")
check(bt_gateway.note_seen("x", "8.8.8.8") is False and bt_gateway.note_seen("x", "no") is False, "note_seen rifiuta indirizzi pubblici o non validi")

print("\n%d ok, %d falliti" % (ok, fail))
sys.exit(1 if fail else 0)
