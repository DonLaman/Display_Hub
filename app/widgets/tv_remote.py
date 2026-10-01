"""
Widget: Telecomando TV (VIDAA) — interattivo (layout button_grid).

Ogni pulsante ha un "icon" (nome, reso dal firmware con le icone di dh_icons)
invece di emoji, che il font del display non disegna. Il testo resta come
etichetta per i pulsanti che ne hanno una (OK, numeri, CH+/-).

Due viste: "main" (tutti i tasti a vista) e "keypad" (tastierino canale).
Le azioni "view:*" cambiano vista sul display; "key:*"/"power:*" vanno al server.
Stili (colori nella mappa btn_style_color del firmware, controllata da tests/test_tv_remote_widget.py):
  arrow = frecce (l'unico blu), power_circle = accensione tonda e rossa al centro della riga,
  channel = ingresso al tastierino, confirm = OK del tastierino, ok = OK del D-pad, back = ritorno.
"""
from typing import Any, Dict, Optional

from app.widgets.base import Widget, WidgetMeta

# main: griglia 4 colonne x 5 righe. icon = nome icona in dh_icons; label opzionale.
_MAIN_BUTTONS = [
    # riga 0: power al centro (cerchio), muto e home ai lati
    {"icon": "power", "action": "power:toggle", "row": 0, "col": 1, "colspan": 2, "style": "power_circle"},
    {"icon": "mute", "label": "Muto", "action": "key:KEY_MUTE", "row": 0, "col": 0},
    {"icon": "home", "label": "Home", "action": "key:KEY_HOME", "row": 0, "col": 3},

    {"icon": "up",      "action": "key:KEY_UP",        "row": 1, "col": 1, "colspan": 2, "style": "arrow"},
    {"icon": "vol_up",  "action": "key:KEY_VOLUMEUP",  "row": 1, "col": 0},
    {"icon": "ch_up", "label": "CH+", "action": "key:KEY_CHANNELUP", "row": 1, "col": 3},

    {"icon": "left",  "action": "key:KEY_LEFT",  "row": 2, "col": 0, "style": "arrow"},
    {"label": "OK",   "action": "key:KEY_OK",    "row": 2, "col": 1, "colspan": 2, "style": "ok"},
    {"icon": "right", "action": "key:KEY_RIGHT", "row": 2, "col": 3, "style": "arrow"},

    {"icon": "vol_down", "action": "key:KEY_VOLUMEDOWN", "row": 3, "col": 0},
    {"icon": "down",     "action": "key:KEY_DOWN",       "row": 3, "col": 1, "colspan": 2, "style": "arrow"},
    {"icon": "ch_down", "label": "CH-", "action": "key:KEY_CHANNELDOWN", "row": 3, "col": 3},

    {"icon": "back", "label": "Indietro", "action": "key:KEY_RETURNS", "row": 4, "col": 0},
    {"icon": "menu", "label": "Menu",     "action": "key:KEY_MENU",    "row": 4, "col": 1},
    {"icon": "keypad", "label": "Canale", "action": "view:keypad",     "row": 4, "col": 2, "colspan": 2, "style": "channel"},
]

_KEYPAD_BUTTONS = [
    {"label": "1", "action": "key:KEY_1", "row": 0, "col": 0},
    {"label": "2", "action": "key:KEY_2", "row": 0, "col": 1},
    {"label": "3", "action": "key:KEY_3", "row": 0, "col": 2},
    {"label": "4", "action": "key:KEY_4", "row": 1, "col": 0},
    {"label": "5", "action": "key:KEY_5", "row": 1, "col": 1},
    {"label": "6", "action": "key:KEY_6", "row": 1, "col": 2},
    {"label": "7", "action": "key:KEY_7", "row": 2, "col": 0},
    {"label": "8", "action": "key:KEY_8", "row": 2, "col": 1},
    {"label": "9", "action": "key:KEY_9", "row": 2, "col": 2},
    {"icon": "back", "action": "view:main", "row": 3, "col": 0, "style": "back"},
    {"label": "0", "action": "key:KEY_0", "row": 3, "col": 1},
    {"label": "OK", "action": "key:KEY_OK", "row": 3, "col": 2, "style": "confirm"},
    {"label": "Torna ai comandi", "action": "view:main", "row": 4, "col": 0, "colspan": 3, "style": "back"},
]


class TvRemoteWidget(Widget):
    meta = WidgetMeta(
        id="tv_remote",
        name="Telecomando TV",
        description="Telecomando touch per la TV Hisense VIDAA: tasti a video, tastierino canale a parte",
        icon="tv",
        refresh_seconds=3600,
        layout_type="button_grid",
    )

    def get_data(self, config: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        return {
            "title": "Telecomando TV",
            "columns": 4,
            "action_endpoint": "/api/esp/tv-action",
            "views": {
                "main": {"columns": 4, "buttons": _MAIN_BUTTONS},
                "keypad": {"columns": 3, "buttons": _KEYPAD_BUTTONS},
            },
            "initial_view": "main",
        }
