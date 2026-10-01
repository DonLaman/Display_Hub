"""Server: tentativi diradati dopo gli errori, ultimo valore buono, log senza
ripetizioni, CoinGecko 429. Esecuzione: python3 tests/test_widgets_resilience.py"""
import io
import logging
import os
import sys
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from app.core import orchestrator  # noqa: E402

ok = fail = 0


def check(c, m):
    global ok, fail
    ok, fail = (ok + 1, fail) if c else (ok, fail + 1)
    print(("OK  " if c else "FAIL"), m)


class FakeWidget:
    class meta:
        layout_type = "big_number"
        refresh_seconds = 60

    def __init__(self):
        self.results = []

    def get_data(self, params):
        r = self.results.pop(0)
        if isinstance(r, Exception):
            raise r
        return r


print("== Ciclo dei widget ==")
w = FakeWidget()
dc = orchestrator.DataCache()
clock = [1000.0]
with mock.patch.object(orchestrator.registry, "get", return_value=w), \
     mock.patch.object(orchestrator.config_store, "get_pages", return_value=[{"page_id": "x", "widget_id": "x", "params": {}}]), \
     mock.patch.object(orchestrator.time, "time", side_effect=lambda: clock[0]):
    w.results = [{"title": "SOL", "value": 121.5}]
    check(dc._update_widget("x") is True and dc.next_delay("x", 60) == 60, "dato buono: intervallo normale")
    w.results = [{"title": "SOL", "value": None, "error": True}]
    clock[0] += 120
    dc._update_widget("x")
    p = dc.get_page("x")["payload"]
    check(p["value"] == 121.5 and p["stale"] and p["stale_seconds"] == 120, "errore: resta l'ultimo prezzo, segnato vecchio")
    check(dc.next_delay("x", 60) == 120, "1 errore: prossimo tentativo tra 120 s")
    for _ in range(3):
        w.results = [{"error": True}]
        dc._update_widget("x")
    check(dc.next_delay("x", 60) == 900, "4 errori: tentativi ogni 15 min al massimo")
    clock[0] += 31 * 60
    w.results = [{"title": "SOL", "value": None, "error": True}]
    dc._update_widget("x")
    check(dc.get_page("x")["payload"].get("value") is None, "dopo 30 min senza dati: si mostra l'errore, non un valore vecchissimo")
    w.results = [RuntimeError("boom")]
    dc._update_widget("x")
    check(dc.get_page("x")["payload"].get("error"), "eccezione del widget: nessun crash del ciclo")
    w.results = [{"title": "SOL", "value": 125.0}]
    dc._update_widget("x")
    check(dc.next_delay("x", 60) == 60 and not dc.get_page("x")["payload"].get("stale"), "di nuovo buono: intervallo normale")

print("\n== Log senza ripetizioni ==")
buf = io.StringIO()
import importlib  # noqa: E402
import app.main as appmain  # noqa: E402,F401
root = logging.getLogger()
flt = next(f for h in root.handlers for f in h.filters if type(f).__name__ == "_RepeatFilter")
h = logging.StreamHandler(buf)
h.addFilter(flt)
lg = logging.getLogger("test.rep")
lg.addHandler(h)
lg.propagate = False
t = [5000.0]
with mock.patch("time.time", side_effect=lambda: t[0]):
    for i in range(60):
        lg.error("WAHA irraggiungibile")
        t[0] += 60
    lg.error("WAHA irraggiungibile")
lines = [l for l in buf.getvalue().splitlines() if l]
check(len(lines) == 2, "60 errori identici in un'ora -> 1 riga (+1 all'ora successiva)")
check("ripetuto altre 59 volte" in lines[-1], "la riga successiva dice quante ripetizioni sono state taciute")
lg.error("altro errore")
check("altro errore" in buf.getvalue(), "un errore diverso compare subito")

print("\n== CoinGecko 429 ==")
from app.widgets import crypto_price  # noqa: E402

crypto_price._retry_after_until = 0
resp = mock.Mock(status_code=429, headers={"Retry-After": "120"})
with mock.patch.object(crypto_price.requests, "get", return_value=resp) as g:
    r = crypto_price.CryptoPriceWidget().get_data({"coin_id": "solana"})
    check(r["error"] and crypto_price._retry_after_until > 0, "429: errore e pausa impostata")
    crypto_price.CryptoPriceWidget().get_data({"coin_id": "solana"})
    check(g.call_count == 1, "durante la pausa nessuna nuova richiesta a CoinGecko")
check(crypto_price.CryptoPriceWidget.meta.refresh_seconds == 60, "aggiornamento ogni 60 s")

print("\n%d ok, %d falliti" % (ok, fail))
sys.exit(1 if fail else 0)
