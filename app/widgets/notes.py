"""Widget generico "testo libero": promemoria, TODO rapido, citazione del giorno.
Esempio del modulo più semplice possibile, utile come base per nuovi widget custom."""
from typing import Any, Dict, Optional

from app.widgets.base import Widget, WidgetMeta


class NotesWidget(Widget):
    meta = WidgetMeta(
        id="notes",
        name="Promemoria",
        description="Testo libero configurabile dalla web UI",
        icon="file-text",
        refresh_seconds=3600,
        layout_type="text_log",
    )

    def get_config_schema(self) -> Dict[str, Any]:
        return {
            "text": {"type": "textarea", "label": "Testo", "default": "Nessun promemoria impostato"},
        }

    def get_data(self, config: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        config = config or {}
        return {"lines": [config.get("text", "Nessun promemoria impostato")]}
