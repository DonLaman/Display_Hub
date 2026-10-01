"""Widget: meteo corrente via Open-Meteo (gratuito, no API key)."""
import logging
from typing import Any, Dict, Optional

import requests

from app.widgets.base import Widget, WidgetMeta

logger = logging.getLogger(__name__)

_WEATHER_CODES = {
    0: "Sereno", 1: "Poco nuvoloso", 2: "Nuvoloso", 3: "Coperto",
    45: "Nebbia", 61: "Pioggia debole", 63: "Pioggia", 65: "Pioggia forte",
    71: "Neve debole", 80: "Rovesci", 95: "Temporale",
}


class WeatherWidget(Widget):
    meta = WidgetMeta(
        id="weather",
        name="Meteo",
        description="Temperatura e condizioni correnti (Open-Meteo)",
        icon="cloud",
        refresh_seconds=600,
        layout_type="big_number",
    )

    def get_config_schema(self) -> Dict[str, Any]:
        return {
            "latitude": {"type": "text", "label": "Latitudine", "default": "41.13"},
            "longitude": {"type": "text", "label": "Longitudine", "default": "16.87"},
            "label": {"type": "text", "label": "Nome località", "default": "Puglia"},
        }

    def get_data(self, config: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        config = config or {}
        lat = config.get("latitude", "41.13")
        lon = config.get("longitude", "16.87")
        label = config.get("label", "Puglia")
        try:
            resp = requests.get(
                "https://api.open-meteo.com/v1/forecast",
                params={"latitude": lat, "longitude": lon, "current_weather": "true"},
                timeout=5,
            )
            resp.raise_for_status()
            cw = resp.json()["current_weather"]
            return {
                "title": f"{label} — {_WEATHER_CODES.get(cw['weathercode'], 'N/D')}",
                "value": round(cw["temperature"]),
                "unit": "°C",
            }
        except Exception as exc:
            logger.error("Errore fetch meteo: %s", exc)
            return {"title": label, "value": None, "unit": "°C", "error": True}
