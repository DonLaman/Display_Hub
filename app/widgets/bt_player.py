"""
Widget: Player Bluetooth (audio verso la cassa tramite il gateway BT).

Come il telecomando TV e' INTERATTIVO (layout button_grid) e ha due viste:
  - "main":    in testa la scheda "in riproduzione" (come un player MP3: fonte, titolo, sottotitolo,
               barra di avanzamento), sotto precedente / play-pausa / successivo, volume e muto,
               stop e "Fonti" (porta alla seconda pagina);
  - "sources": la scelta della fonte (MP3, YouTube, Web radio). SEGNAPOSTO: si sceglie, ma la
               riproduzione non e' ancora sviluppata (vedi core/audio_player.py).
Le azioni "view:*" cambiano pagina sul display; "source:*;view:main" sceglie la fonte e torna al
player. Le altre vanno al server (/api/esp/audio-action), che le gira al gateway.
Parametro: gateway_id (vuoto = il primo gateway configurato).
"""
from typing import Any, Dict, Optional

from app.core import audio_player
from app.widgets.base import Widget, WidgetMeta

_MAIN_BUTTONS = [
    {"icon": "prev", "action": "audio:prev", "row": 0, "col": 0},
    {"icon": "play_pause", "action": "audio:play_pause", "row": 0, "col": 1, "colspan": 2, "style": "play_circle"},
    {"icon": "next", "action": "audio:next", "row": 0, "col": 3},

    {"icon": "vol_down", "action": "vol:down", "row": 1, "col": 0},
    {"icon": "mute", "label": "Muto", "action": "vol:mute", "row": 1, "col": 1, "colspan": 2},
    {"icon": "vol_up", "action": "vol:up", "row": 1, "col": 3},

    {"icon": "stop", "label": "Stop", "action": "audio:stop", "row": 2, "col": 0, "colspan": 2},
    {"icon": "menu", "label": "Fonti", "action": "view:sources", "row": 2, "col": 2, "colspan": 2, "style": "channel"},
]

# Fonti segnaposto: "inline" = icona e testo affiancati (bottoni larghi).
_SOURCE_BUTTONS = [
    {"icon": "music", "label": "MP3 - in arrivo", "action": "source:mp3;view:main", "row": 0, "col": 0, "style": "src_mp3", "inline": True},
    {"icon": "youtube", "label": "YouTube - in arrivo", "action": "source:youtube;view:main", "row": 1, "col": 0, "style": "src_youtube", "inline": True},
    {"icon": "radio", "label": "Web radio - in arrivo", "action": "source:radio;view:main", "row": 2, "col": 0, "style": "src_radio", "inline": True},
    {"icon": "back", "label": "Torna al player", "action": "view:main", "row": 3, "col": 0, "style": "back", "inline": True},
]


class BtPlayerWidget(Widget):
    meta = WidgetMeta(
        id="bt_player",
        name="Player Bluetooth",
        description="Player audio per la cassa Bluetooth: in riproduzione, controlli, volume e scelta della fonte (MP3 / YouTube / Web radio, segnaposto)",
        icon="speaker",
        refresh_seconds=3,     # il volume e la fonte scelta compaiono nella scheda entro pochi secondi
        layout_type="button_grid",
    )

    def get_config_schema(self) -> Dict[str, Any]:
        return {"gateway_id": {"type": "text", "label": "Gateway Bluetooth (id, vuoto = il primo)", "default": ""}}

    def get_data(self, config: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        requested = ((config or {}).get("gateway_id") or "").strip()
        gid = audio_player.resolve_gateway(requested)
        endpoint = "/api/esp/audio-action" + (f"?gateway={gid}" if requested else "")
        return {
            "title": "Audio Bluetooth",
            "columns": 4,
            "action_endpoint": endpoint,
            "views": {
                "main": {"columns": 4, "header": audio_player.header(gid), "buttons": _MAIN_BUTTONS},
                "sources": {"columns": 1, "buttons": _SOURCE_BUTTONS},
            },
            "initial_view": "main",
        }
