"""Widget: orologio e data.

Il display fa avanzare l'ora da solo (i dati dei widget arrivano ogni pochi
minuti): qui si manda l'ora esatta ("epoch", UTC) e lo scarto del fuso orario
("tz_offset", ora legale compresa), più i testi già pronti per chi non li usa.
Giorni e mesi in italiano, indipendenti dalla locale del container.

Fuso orario: DH_TIMEZONE (default Europe/Rome), con le tabelle del pacchetto
Python "tzdata": il container python:slim gira in UTC e non ha
/usr/share/zoneinfo, quindi non ci si affida al fuso del sistema.
"""
import datetime
import os
import time
from typing import Any, Dict, Optional

from app.widgets.base import Widget, WidgetMeta

try:
    from zoneinfo import ZoneInfo
except ImportError:  # pragma: no cover
    ZoneInfo = None

DEFAULT_TZ = "Europe/Rome"
GIORNI = ["lunedì", "martedì", "mercoledì", "giovedì", "venerdì", "sabato", "domenica"]
MESI = ["gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno",
        "luglio", "agosto", "settembre", "ottobre", "novembre", "dicembre"]


def _now() -> datetime.datetime:
    name = os.environ.get("DH_TIMEZONE", DEFAULT_TZ)
    if ZoneInfo is not None:
        try:
            return datetime.datetime.now(ZoneInfo(name))
        except Exception:  # fuso sconosciuto o tzdata mancante: ora del sistema
            pass
    return datetime.datetime.now().astimezone()


class ClockWidget(Widget):
    meta = WidgetMeta(
        id="clock",
        name="Orologio",
        description="Data e ora correnti: il display le fa avanzare da solo",
        icon="clock",
        refresh_seconds=300,
        layout_type="big_number",
    )

    def get_data(self, config: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        epoch = int(time.time())
        now = _now()
        offset = now.utcoffset()
        return {
            "title": f"{GIORNI[now.weekday()]} {now.day} {MESI[now.month - 1]}",
            "value": now.strftime("%H:%M"),
            "unit": "",
            "epoch": epoch,
            "tz_offset": int(offset.total_seconds()) if offset else 0,
        }
