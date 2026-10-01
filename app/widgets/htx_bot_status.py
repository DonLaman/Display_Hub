"""
Widget: stato del bot di trading HTX (progetto esistente su ryzen_server).
Presuppone che htx_bot esponga un piccolo endpoint di stato interno
(es. http://htx_bot:8000/status) sulla rete Docker condivisa: da adattare
all'endpoint reale del tuo bot.
"""
import logging
from typing import Any, Dict, Optional

import requests

from app.widgets.base import Widget, WidgetMeta

logger = logging.getLogger(__name__)


class HtxBotStatusWidget(Widget):
    meta = WidgetMeta(
        id="htx_bot_status",
        name="Stato Bot HTX",
        description="Stato operativo del bot di trading algoritmico su HTX",
        icon="activity",
        refresh_seconds=15,
        layout_type="status_grid",
    )

    def get_config_schema(self) -> Dict[str, Any]:
        return {
            "status_url": {"type": "text", "label": "URL endpoint stato",
                            "default": "http://htx_bot:8000/status"},
        }

    def get_data(self, config: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        config = config or {}
        url = config.get("status_url", "http://htx_bot:8000/status")
        try:
            resp = requests.get(url, timeout=5)
            resp.raise_for_status()
            status = resp.json()
            return {
                "items": [
                    {"label": "Stato", "value": "Attivo" if status.get("running") else "Fermo"},
                    {"label": "Simboli attivi", "value": status.get("active_symbols_count", "-")},
                    {"label": "P&L oggi", "value": f"{status.get('pnl_today', 0):+.2f} USDT"},
                    {"label": "Ultima op.", "value": status.get("last_trade_time", "-")},
                ]
            }
        except Exception as exc:
            logger.error("Errore fetch stato bot HTX: %s", exc)
            return {"items": [{"label": "Stato", "value": "Non raggiungibile"}], "error": True}

    def health_check(self) -> bool:
        return True  # verifica reale delegata a get_data(); qui evitiamo doppie chiamate
