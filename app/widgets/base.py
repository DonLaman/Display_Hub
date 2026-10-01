"""
Contratto base per tutti i widget/pagine del display.

Ogni modulo "unico" (es. btc_price.py, htx_bot.py, docker_health.py) deve
definire UNA classe che eredita da Widget e implementa get_data().
Il core (registry.py) scopre automaticamente i widget presenti in questa
cartella: per aggiungerne uno nuovo basta creare il file, non serve
registrarlo manualmente altrove.
"""
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Any, Dict, Optional


@dataclass
class WidgetMeta:
    """Metadati statici del widget, usati dalla web UI per l'elenco selezionabile."""
    id: str                     # identificativo univoco, es. "btc_price"
    name: str                   # nome leggibile, es. "Prezzo BTC"
    description: str = ""
    icon: str = "square"        # nome icona (usata solo lato web UI)
    refresh_seconds: int = 30   # ogni quanto l'ESP32 dovrebbe rifare polling dei dati
    # Tipo di layout che il firmware deve renderizzare per questa pagina.
    # Il firmware ha un renderer generico per ciascun layout_type.
    layout_type: str = "kv_list"   # kv_list | big_number | status_grid | gauge | text_log


class Widget(ABC):
    """Classe base: ogni widget concreto vive nel proprio file, isolato dagli altri."""

    meta: WidgetMeta

    @abstractmethod
    def get_data(self, config: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        """
        Ritorna il payload dati per questo widget, in un formato JSON-serializzabile
        già pronto per essere inviato al display. La struttura interna dipende dal
        layout_type dichiarato in meta (vedi PROTOCOL.md).
        """
        raise NotImplementedError

    def get_config_schema(self) -> Dict[str, Any]:
        """
        Opzionale: se il widget ha parametri configurabili dalla web UI
        (es. quale simbolo mostrare, quale container monitorare), li dichiara qui
        come schema semplice {campo: {type, label, default}}.
        """
        return {}

    def health_check(self) -> bool:
        """Opzionale: usato dalla web UI per mostrare se le dipendenze del widget sono raggiungibili."""
        return True
