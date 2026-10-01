"""Widget: metriche del portale W-BOT (messaggi inviati oggi, tenant attivi).
Esempio da adattare all'endpoint reale del progetto w-bot-client-portal."""
import logging
from typing import Any, Dict, Optional

import requests

from app.widgets.base import Widget, WidgetMeta

logger = logging.getLogger(__name__)


class WBotMetricsWidget(Widget):
    meta = WidgetMeta(
        id="wbot_metrics",
        name="Metriche W-BOT",
        description="Messaggi WhatsApp inviati oggi e tenant attivi",
        icon="message-circle",
        refresh_seconds=60,
        layout_type="status_grid",
    )

    def get_config_schema(self) -> Dict[str, Any]:
        return {
            "metrics_url": {"type": "text", "label": "URL endpoint metriche",
                             "default": "http://w-bot:5000/api/internal/metrics"},
        }

    def get_data(self, config: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        config = config or {}
        url = config.get("metrics_url", "http://w-bot:5000/api/internal/metrics")
        try:
            resp = requests.get(url, timeout=5)
            resp.raise_for_status()
            data = resp.json()
            return {
                "items": [
                    {"label": "Messaggi oggi", "value": data.get("messages_today", "-")},
                    {"label": "Tenant attivi", "value": data.get("active_tenants", "-")},
                ]
            }
        except Exception as exc:
            logger.error("Errore fetch metriche W-BOT: %s", exc)
            return {"items": [{"label": "Stato", "value": "Non raggiungibile"}], "error": True}
