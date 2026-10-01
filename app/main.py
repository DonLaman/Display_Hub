"""
Entry point Flask.

Espone:
- la web UI (configurazione pagine + gestione flashing USB)
- /api/*      -> usate dal browser (config, widget, flashing)
- /api/esp/*  -> usate dal DISPLAY via WiFi (polling dati, registrazione)

Nessuna comunicazione seriale a runtime: il flashing avviene interamente nel
browser via Web Serial (ESP Web Tools), il server si limita a conservare i
firmware caricati e a generarne il manifest (core/firmware_store.py).
"""
import logging

from flask import Flask

from app.core import auth
from app.core.orchestrator import data_cache
from app.routes import api_bp, esp_bp, ui_bp

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s [%(name)s] %(message)s")


class _RepeatFilter(logging.Filter):
    """Un errore identico ripetuto (es. un servizio irraggiungibile, ogni
    minuto) finisce nel log una volta sola ogni REPEAT_WINDOW secondi, con il
    conteggio delle ripetizioni nel frattempo, invece di riempirlo."""

    REPEAT_WINDOW = 3600

    def __init__(self):
        super().__init__()
        self._seen = {}  # (logger, messaggio) -> [primo_istante_finestra, ripetizioni_taciute]

    def filter(self, record):
        if record.levelno < logging.WARNING:
            return True
        key = (record.name, record.getMessage())
        import time as _t
        now = _t.time()
        entry = self._seen.get(key)
        if entry and now - entry[0] < self.REPEAT_WINDOW:
            entry[1] += 1
            return False
        if entry and entry[1]:
            record.msg = "%s  [ripetuto altre %d volte nell'ultima ora]" % (record.getMessage(), entry[1])
            record.args = ()
        self._seen[key] = [now, 0]
        if len(self._seen) > 500:  # niente crescita illimitata
            self._seen.clear()
        return True


for _h in logging.getLogger().handlers:
    _h.addFilter(_RepeatFilter())


def create_app() -> Flask:
    app = Flask(__name__)
    # Login della web UI (utente/password dal container, sessione in cookie).
    # Le API dei display (/api/esp) restano senza login ma solo dirette: vedi core/auth.py.
    auth.init_app(app)
    app.register_blueprint(ui_bp)
    app.register_blueprint(api_bp, url_prefix="/api")
    app.register_blueprint(esp_bp, url_prefix="/api/esp")

    # Avvia il calcolo periodico dei dati dei widget attivi, indipendentemente
    # da quanti/se display sono connessi in quel momento
    data_cache.start()

    return app


app = create_app()

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=12000, debug=False)
