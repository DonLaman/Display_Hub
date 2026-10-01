"""
Scopre automaticamente tutti i widget in app/widgets/*.py (esclusi base.py e __init__.py),
li istanzia e li espone come registro {widget_id: istanza}.

Aggiungere una nuova funzionalità = aggiungere un file in widgets/, nient'altro.
"""
import importlib
import inspect
import logging
import pkgutil
from typing import Dict

from app.widgets.base import Widget

logger = logging.getLogger(__name__)

_EXCLUDED_MODULES = {"base", "__init__"}


class WidgetRegistry:
    def __init__(self):
        self._widgets: Dict[str, Widget] = {}
        self._load_widgets()

    def _load_widgets(self):
        import app.widgets as widgets_pkg

        for _, module_name, _ in pkgutil.iter_modules(widgets_pkg.__path__):
            if module_name in _EXCLUDED_MODULES:
                continue
            try:
                module = importlib.import_module(f"app.widgets.{module_name}")
            except Exception as exc:
                logger.error("Impossibile caricare il widget '%s': %s", module_name, exc)
                continue

            for _, obj in inspect.getmembers(module, inspect.isclass):
                if issubclass(obj, Widget) and obj is not Widget and obj.__module__ == module.__name__:
                    try:
                        instance = obj()
                        self._widgets[instance.meta.id] = instance
                        logger.info("Widget caricato: %s (%s)", instance.meta.id, module_name)
                    except Exception as exc:
                        logger.error("Errore istanziando widget in '%s': %s", module_name, exc)

    def all(self) -> Dict[str, Widget]:
        return self._widgets

    def get(self, widget_id: str) -> Widget:
        if widget_id not in self._widgets:
            raise KeyError(f"Widget '{widget_id}' non trovato")
        return self._widgets[widget_id]

    def list_meta(self):
        return [w.meta for w in self._widgets.values()]


# Istanza singleton usata da tutta l'app
registry = WidgetRegistry()
