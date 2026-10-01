"""Widget: utilizzo CPU/RAM/disco del ryzen_server (psutil, letto dall'host se montato,
altrimenti dal container stesso)."""
from typing import Any, Dict, Optional

import psutil

from app.widgets.base import Widget, WidgetMeta


class ServerResourcesWidget(Widget):
    meta = WidgetMeta(
        id="server_resources",
        name="Risorse Server",
        description="Uso CPU, RAM e disco del ryzen_server",
        icon="cpu",
        refresh_seconds=15,
        layout_type="status_grid",
    )

    def get_data(self, config: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        cpu = psutil.cpu_percent(interval=0.5)
        mem = psutil.virtual_memory()
        disk = psutil.disk_usage("/")
        return {
            "items": [
                {"label": "CPU", "value": f"{cpu:.0f}%"},
                {"label": "RAM", "value": f"{mem.percent:.0f}%"},
                {"label": "Disco", "value": f"{disk.percent:.0f}%"},
            ]
        }
