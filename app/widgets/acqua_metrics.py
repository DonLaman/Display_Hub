"""Widget: metriche del progetto Acqua (letture in coda, bollette generate)."""
import logging
from typing import Any, Dict, Optional

import requests

from app.widgets.base import Widget, WidgetMeta

logger = logging.getLogger(__name__)


class AcquaMetricsWidget(Widget):
    meta = WidgetMeta(
        id="acqua_metrics",
        name="Metriche Acqua",
        description="Letture contatori in coda e bollette generate",
        icon="droplet",
        refresh_seconds=120,
        layout_type="status_grid",
    )

    def get_config_schema(self) -> Dict[str, Any]:
        return {
            "metrics_url": {"type": "text", "label": "URL endpoint metriche",
                             "default": "http://acqua:8501/api/internal/metrics"},
        }

    def get_data(self, config: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        config = config or {}
        url = config.get("metrics_url", "http://acqua:8501/api/internal/metrics")
        try:
            resp = requests.get(url, timeout=5)
            resp.raise_for_status()
            data = resp.json()
            return {
                "items": [
                    {"label": "Letture in coda", "value": data.get("pending_readings", "-")},
                    {"label": "Bollette mese", "value": data.get("bills_this_month", "-")},
                ]
            }
        except Exception as exc:
            logger.error("Errore fetch metriche Acqua: %s", exc)
            return {"items": [{"label": "Stato", "value": "Non raggiungibile"}], "error": True}
