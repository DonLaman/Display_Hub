"""Widget: stato sessione WAHA Responder."""
import logging
from typing import Any, Dict, Optional

import requests

from app.widgets.base import Widget, WidgetMeta

logger = logging.getLogger(__name__)


class WahaStatusWidget(Widget):
    meta = WidgetMeta(
        id="waha_status",
        name="Stato WAHA",
        description="Stato della sessione WhatsApp e risposte automatiche inviate",
        icon="message-square",
        refresh_seconds=60,
        layout_type="status_grid",
    )

    def get_config_schema(self) -> Dict[str, Any]:
        return {
            "status_url": {"type": "text", "label": "URL endpoint stato",
                            "default": "http://waha-responder:5000/api/internal/status"},
        }

    def get_data(self, config: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        config = config or {}
        url = config.get("status_url", "http://waha-responder:5000/api/internal/status")
        try:
            resp = requests.get(url, timeout=5)
            resp.raise_for_status()
            data = resp.json()
            return {
                "items": [
                    {"label": "Sessione", "value": data.get("session_state", "-")},
                    {"label": "Risposte oggi", "value": data.get("replies_today", "-")},
                ]
            }
        except requests.exceptions.ConnectionError as exc:
            if "resolve" in str(exc) or "Name or service not known" in str(exc):
                host = requests.utils.urlparse(url).hostname
                logger.error(
                    "WAHA: il nome '%s' non si risolve dal container di Display Hub. Due strade: collegare "
                    "il container alla stessa rete docker di WAHA (docker-compose.yml, sezione 'networks', "
                    "es. ryzen_shared) oppure impostare nella pagina del widget l'URL con IP e porta "
                    "pubblicata (es. http://192.168.1.40:5000/api/internal/status).", host)
            else:
                logger.error("Errore fetch stato WAHA: %s", exc)
            return {"items": [{"label": "Stato", "value": "Non raggiungibile"}], "error": True}
        except Exception as exc:
            logger.error("Errore fetch stato WAHA: %s", exc)
            return {"items": [{"label": "Stato", "value": "Non raggiungibile"}], "error": True}
