"""
Ricalcola periodicamente i dati di ogni pagina attiva (ognuna al proprio
refresh_seconds) e li tiene in una cache in memoria. Gli endpoint /api/esp/*
serviranno questa cache ai display che fanno polling via WiFi: il display
non aspetta mai il tempo di calcolo di un widget (es. una chiamata HTTP lenta
a un'API esterna), riceve sempre l'ultimo valore già pronto.

Ha anche un "config_version": incrementato ad ogni salvataggio pagine dalla
web UI, così il firmware capisce se deve ricostruire gli schermi o gli basta
aggiornare i dati di quelli che già ha.
"""
import logging
import threading
import time
from typing import Any, Dict, Optional

from app.core import config_store
from app.core.registry import registry

logger = logging.getLogger(__name__)

# Dopo un errore di un widget (servizio giù, API che risponde "troppe
# richieste"...) il prossimo tentativo si allontana: intervallo x2, x4, ...
# fino a MAX_BACKOFF_S. Al primo successo si torna all'intervallo normale.
MAX_BACKOFF_S = 15 * 60
# Per quanto si continua a mostrare l'ULTIMO VALORE BUONO (segnato "stale")
# invece dell'errore: una API momentaneamente irraggiungibile non deve far
# sparire il prezzo o il meteo dal display.
STALE_GRACE_S = 30 * 60


class DataCache:
    def __init__(self):
        self._lock = threading.Lock()
        self._cache: Dict[str, Dict[str, Any]] = {}   # widget_id -> {"layout_type", "payload", "updated_at"}
        self._config_version = 0
        self._poll_threads: Dict[str, threading.Thread] = {}
        self._stop = threading.Event()
        self._last_good: Dict[str, Dict[str, Any]] = {}  # widget_id -> {"payload", "at"}
        self._failures: Dict[str, int] = {}              # errori consecutivi per widget

    @property
    def config_version(self) -> int:
        return self._config_version

    def get_snapshot(self):
        """Ritorna {config_version, server_time, pages: [{page_id, widget_id, layout_type, payload}]}
        nell'ordine configurato: è la risposta unica che il display riceve ad ogni poll.
        server_time: ora esatta del server (UTC), per l'orologio del display: i
        dati dei widget possono avere qualche minuto, questa no."""
        pages = config_store.get_pages()
        with self._lock:
            result = []
            for page in pages:
                entry = self._cache.get(page["page_id"])
                if entry is None:
                    continue
                result.append({
                    "page_id": page["page_id"],
                    "widget_id": page["widget_id"],
                    "layout_type": entry["layout_type"],
                    "payload": entry["payload"],
                })
            return {"config_version": self._config_version, "server_time": int(time.time()), "pages": result}

    def get_page(self, page_id: str):
        with self._lock:
            return self._cache.get(page_id)

    def _update_widget(self, page_id: str) -> Optional[bool]:
        """Aggiorna i dati di UNA pagina (widget + i suoi parametri); True se buoni, False se errore."""
        page = next((p for p in config_store.get_pages() if p["page_id"] == page_id), None)
        if page is None:
            return None
        try:
            widget = registry.get(page["widget_id"])
        except KeyError:
            return None
        params = page.get("params", {})
        widget_id = page["widget_id"]
        try:
            payload = widget.get_data(params)
        except Exception as exc:
            logger.error("Errore get_data() per widget '%s': %s", widget_id, exc)
            payload = {"error": True}
        failed = bool(isinstance(payload, dict) and payload.get("error"))
        now = time.time()
        if failed:
            self._failures[page_id] = self._failures.get(page_id, 0) + 1
            good = self._last_good.get(page_id)
            if good and now - good["at"] < STALE_GRACE_S:
                # Ultimo valore buono, segnato come vecchio (con l'età).
                payload = dict(good["payload"])
                payload["stale"] = True
                payload["stale_seconds"] = int(now - good["at"])
            elif payload == {"error": True}:
                return False  # eccezione e nessun valore da mostrare: resta quello in cache
        else:
            if self._failures.get(page_id):
                logger.info("Widget '%s' di nuovo disponibile dopo %d errori", widget_id, self._failures[page_id])
            self._failures[page_id] = 0
            self._last_good[page_id] = {"payload": payload, "at": now}
        with self._lock:
            self._cache[page_id] = {
                "layout_type": widget.meta.layout_type,
                "payload": payload,
                "updated_at": now,
            }
        return not failed

    def next_delay(self, page_id: str, interval: float) -> float:
        """Intervallo fino al prossimo aggiornamento, allungato dopo gli errori."""
        n = self._failures.get(page_id, 0)
        if n <= 0:
            return interval
        return min(interval * (2 ** min(n, 10)), max(MAX_BACKOFF_S, interval))

    @staticmethod
    def preview(widget_id: str, params: Dict[str, Any]) -> Dict[str, Any]:
        """Anteprima con parametri qualsiasi (web UI): calcolata al volo, non tocca la cache."""
        widget = registry.get(widget_id)
        return widget.get_data(params or {})

    def apply_config_change(self):
        """Chiamato dalla web UI dopo ogni salvataggio pagine: incrementa la versione
        e riavvia i loop di polling coerenti con le pagine correnti."""
        self._config_version += 1
        self._stop.set()
        self._stop = threading.Event()
        stop_flag = self._stop

        pages = config_store.get_pages()
        current = {p["page_id"] for p in pages}
        with self._lock:  # dati di pagine tolte: via
            for pid in list(self._cache):
                if pid not in current:
                    del self._cache[pid]
        self._poll_threads = {}
        for page in pages:
            try:
                widget = registry.get(page["widget_id"])
            except KeyError:
                continue
            interval = widget.meta.refresh_seconds

            def loop(pid=page["page_id"], interval=interval, flag=stop_flag):
                while not flag.is_set():
                    self._update_widget(pid)
                    flag.wait(self.next_delay(pid, interval))

            t = threading.Thread(target=loop, daemon=True)
            t.start()
            self._poll_threads[page["page_id"]] = t

    def start(self):
        self.apply_config_change()


data_cache = DataCache()
