"""Pagine: stesso widget più volte con parametri diversi, identificativi di
pagina, ora esatta nel poll, widget "Prezzi crypto (elenco)".
Esecuzione: python3 tests/test_pages.py"""
import json
import os
import sys
import tempfile
import time
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from app.core import config_store, orchestrator  # noqa: E402
from app.widgets import crypto_price, crypto_watchlist  # noqa: E402

ok = fail = 0


def check(c, m):
    global ok, fail
    ok, fail = (ok + 1, fail) if c else (ok, fail + 1)
    print(("OK  " if c else "FAIL"), m)


PRICES = {"bitcoin": {"usd": 65432.1, "usd_24h_change": 1.234}, "ethereum": {"usd": 3456.7, "usd_24h_change": -2.5},
          "solana": {"usd": 121.29, "usd_24h_change": 0.4}, "ripple": {"usd": 0.5123, "usd_24h_change": -0.1},
          "tether-gold": {"usd": 4688.82, "usd_24h_change": 3.0}}


def fake_get(url, params=None, timeout=None):
    ids = params["ids"].split(",")
    r = mock.Mock(status_code=200)
    r.json.return_value = {i: PRICES[i] for i in ids if i in PRICES}
    r.raise_for_status = lambda: None
    return r


tmp = tempfile.mkdtemp()
config_store._CONFIG_PATH = os.path.join(tmp, "config.json")

print("== Identificativi di pagina ==")
with open(config_store._CONFIG_PATH, "w") as f:  # configurazione di prima: senza page_id
    json.dump({"pages": [{"widget_id": "crypto_price", "params": {"coin_id": "solana"}},
                         {"widget_id": "crypto_price", "params": {"coin_id": "bitcoin"}}]}, f)
pages = config_store.get_pages()
ids = [p["page_id"] for p in pages]
check(len(set(ids)) == 2 and all(ids), "configurazione vecchia: identificativi assegnati")
check(json.load(open(config_store._CONFIG_PATH))["pages"][0]["page_id"] == ids[0], "e salvati una volta sola")
config_store.set_pages([{"widget_id": "clock", "page_id": "aaaa"}, {"widget_id": "clock", "page_id": "aaaa"}])
p2 = config_store.get_pages()
check(p2[0]["page_id"] == "aaaa" and p2[1]["page_id"] != "aaaa" and p2[1]["params"] == {}, "duplicati: nuovo identificativo")

print("\n== Stesso widget, parametri diversi ==")
config_store.set_pages([{"widget_id": "crypto_price", "params": {"coin_id": "solana", "vs_currency": "usd"}},
                        {"widget_id": "crypto_price", "params": {"coin_id": "bitcoin", "vs_currency": "usd"}}])
dc = orchestrator.DataCache()
crypto_price._retry_after_until = 0
with mock.patch.object(crypto_price.requests, "get", side_effect=fake_get):
    for p in config_store.get_pages():
        dc._update_widget(p["page_id"])
snap = dc.get_snapshot()
titles = [p["payload"]["title"] for p in snap["pages"]]
check(titles == ["SOLANA", "BITCOIN"], "due pagine Prezzo Crypto: ognuna la sua moneta (%s)" % titles)
check(all(p.get("page_id") for p in snap["pages"]), "il poll riporta page_id")
check(abs(snap["server_time"] - time.time()) < 2, "il poll riporta l'ora esatta del server")
config_store.set_pages(config_store.get_pages()[1:])  # tolta la prima
dc.apply_config_change()
dc._stop.set()
check([p["payload"]["title"] for p in dc.get_snapshot()["pages"]] == ["BITCOIN"], "pagina tolta: i suoi dati spariscono")

print("\n== Prezzi crypto (elenco) ==")
w = crypto_watchlist.CryptoWatchlistWidget()
with mock.patch.object(crypto_watchlist.requests, "get", side_effect=fake_get) as g:
    d = w.get_data({"coins": ["bitcoin", "ethereum", "solana", "ripple", "tether-gold"], "vs_currency": "usd"})
    check(g.call_count == 1, "una sola richiesta per 5 monete")
    labels = [i["label"] for i in d["items"]]
    check(labels == ["BTC", "ETH", "SOL", "XRP", "XAUT"], "sigle: %s" % labels)
    btc = d["items"][0]
    check(btc["value"] == "65.432,10 $  +1,2%" and btc["color"] == "#3DDC84", "formato italiano e variazione verde: %r" % btc["value"])
    check(d["items"][1]["color"] == "#FF6B6B" and "-2,5%" in d["items"][1]["value"], "variazione negativa in rosso")
    check(d["items"][3]["value"].startswith("0,5123"), "prezzi piccoli con 4 decimali")
    d2 = w.get_data({"coins": "bitcoin", "extra_coins": "solana, nonesiste", "vs_currency": "usd"})
    check([i["label"] for i in d2["items"]] == ["BTC", "SOL", "NONESIST"] and d2["items"][2]["value"] == "id sconosciuto",
          "monete aggiunte a mano, id sbagliato segnalato")
    check(len(w.get_data({})["items"]) == 5, "senza parametri: le 5 predefinite")
crypto_price._retry_after_until = 0
resp429 = mock.Mock(status_code=429, headers={"Retry-After": "90"})
with mock.patch.object(crypto_watchlist.requests, "get", return_value=resp429):
    d3 = w.get_data({})
check(d3.get("error") and crypto_price._retry_after_until > time.time(), "429: pausa condivisa con Prezzo Crypto")
with mock.patch.object(crypto_price.requests, "get", side_effect=AssertionError("non deve chiamare")):
    check(crypto_price.CryptoPriceWidget().get_data({"coin_id": "solana"}).get("error"), "durante la pausa neanche Prezzo Crypto chiama")
crypto_price._retry_after_until = 0

print("\n== API ==")
from app.main import create_app  # noqa: E402

c = create_app().test_client()
with mock.patch.object(crypto_watchlist.requests, "get", side_effect=fake_get):
    r = c.post("/api/widgets/crypto_watchlist/preview", json={"params": {"coins": ["ethereum"]}})
check(r.status_code == 200 and r.get_json()["items"][0]["label"] == "ETH", "anteprima con i parametri scelti")
r = c.post("/api/pages", json=[{"widget_id": "crypto_watchlist", "params": {"coins": ["bitcoin"]}},
                               {"widget_id": "crypto_price", "params": {"coin_id": "ripple"}}])
pages = r.get_json()["pages"]
check(r.status_code == 200 and all(p.get("page_id") for p in pages), "salvataggio: identificativi restituiti alla web UI")
check(c.post("/api/pages", json=[{"widget_id": "clock", "params": "x"}]).status_code == 400, "params non oggetto: rifiutato")
schema = {w["id"]: w for w in c.get("/api/widgets").get_json()}["crypto_watchlist"]["config_schema"]
check(schema["coins"]["type"] == "multiselect" and "tether-gold" in schema["coins"]["options"], "schema: scelta multipla delle monete")

print("\n%d ok, %d falliti" % (ok, fail))
sys.exit(1 if fail else 0)
