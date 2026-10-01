"""
Widget: stato dei container Docker sul ryzen_server, letto tramite il socket
Docker montato nel container di questa app (vedi docker-compose.yml:
/var/run/docker.sock:/var/run/docker.sock).
"""
import logging
from typing import Any, Dict, Optional

from app.widgets.base import Widget, WidgetMeta

logger = logging.getLogger(__name__)

try:
    import docker
    _DOCKER_AVAILABLE = True
except ImportError:
    _DOCKER_AVAILABLE = False


class DockerHealthWidget(Widget):
    meta = WidgetMeta(
        id="docker_health",
        name="Stato Docker",
        description="Container up/down sul server (via Docker socket)",
        icon="box",
        refresh_seconds=20,
        layout_type="status_grid",
    )

    def get_data(self, config: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        if not _DOCKER_AVAILABLE:
            return {"items": [{"label": "Errore", "value": "libreria docker non installata"}], "error": True}
        try:
            client = docker.from_env()
            containers = client.containers.list(all=True)
            running = sum(1 for c in containers if c.status == "running")
            stopped = len(containers) - running
            items = [
                {"label": "In esecuzione", "value": running},
                {"label": "Fermi", "value": stopped},
            ]
            # Aggiunge i primi 3 container fermi per nome, utile per capire cosa guardare
            down_names = [c.name for c in containers if c.status != "running"][:3]
            for name in down_names:
                items.append({"label": "Fermo", "value": name})
            return {"items": items}
        except Exception as exc:
            logger.error("Errore lettura stato Docker: %s", exc)
            return {"items": [{"label": "Errore", "value": "Docker non raggiungibile"}], "error": True}
