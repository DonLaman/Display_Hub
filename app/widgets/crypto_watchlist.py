"""
Widget: più criptovalute in una sola pagina (es. BTC, ETH, SOL, XRP, oro
tokenizzato), con prezzo e variazione 24h colorata.

UNA sola richiesta a CoinGecko per tutte le monete (/simple/price accetta più
id): molto meglio di una pagina per moneta, visti i limiti dell'API gratuita
per indirizzo IP. Se CoinGecko risponde "troppe richieste" si rispetta la
pausa richiesta (condivisa con il widget "Prezzo Crypto").

Oro: "tether-gold" è Tether Gold (XAUT); XAUt0 è la sua versione multi-chain,
stesso oro e stesso prezzo.
"""
import logging
import time
from typing import Any, Dict, List, Optional

import requests

from app.widgets import crypto_price
from app.widgets.base import Widget, WidgetMeta

logger = logging.getLogger(__name__)

# id CoinGecko -> sigla mostrata sul display
KNOWN = {
    "bitcoin": "BTC",
    "ethereum": "ETH",
    "solana": "SOL",
    "ripple": "XRP",
    "tether-gold": "XAUT",
    "binancecoin": "BNB",
    "cardano": "ADA",
    "dogecoin": "DOGE",
    "pax-gold": "PAXG",
}
DEFAULT_COINS = ["bitcoin", "ethereum", "solana", "ripple", "tether-gold"]
SYMBOL = {"usd": "$", "eur": "€"}
UP, DOWN = "#3DDC84", "#FF6B6B"


def _coins(params: Dict[str, Any]) -> List[str]:
    """Monete scelte (elenco o testo separato da virgole) + quelle aggiunte a mano."""
    out: List[str] = []
    for key in ("coins", "extra_coins"):
        v = params.get(key)
        items = v if isinstance(v, list) else str(v or "").split(",")
        for c in items:
            c = str(c).strip().lower()
            if c and c not in out:
                out.append(c)
    return out or list(DEFAULT_COINS)


def _fmt_price(v: float) -> str:
    """Formato italiano: 65.432,10 — più decimali per prezzi piccoli."""
    decimals = 2 if v >= 1 else 4
    s = f"{v:,.{decimals}f}"
    return s.replace(",", "X").replace(".", ",").replace("X", ".")


class CryptoWatchlistWidget(Widget):
    meta = WidgetMeta(
        id="crypto_watchlist",
        name="Prezzi crypto (elenco)",
        description="Più criptovalute in una pagina: prezzo e variazione 24h (CoinGecko, una sola richiesta)",
        icon="list",
        refresh_seconds=60,
        layout_type="status_grid",
    )

    def get_config_schema(self) -> Dict[str, Any]:
        return {
            "coins": {"type": "multiselect", "label": "Monete", "default": DEFAULT_COINS,
                      "options": list(KNOWN.keys()),
                      "labels": {k: f"{v} ({k})" for k, v in KNOWN.items()}},
            "extra_coins": {"type": "text", "label": "Altre monete (id CoinGecko, separati da virgola)",
                            "default": ""},
            "vs_currency": {"type": "select", "label": "Valuta", "default": "usd", "options": ["usd", "eur"]},
        }

    def get_data(self, config: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        config = config or {}
        coins = _coins(config)[:8]  # sul display ci stanno 8 righe
        vs = str(config.get("vs_currency") or "usd").lower()
        if time.time() < crypto_price._retry_after_until:
            return {"items": [{"label": "CoinGecko", "value": "in pausa (troppe richieste)"}], "error": True}
        try:
            resp = requests.get(crypto_price._COINGECKO_URL,
                                params={"ids": ",".join(coins), "vs_currencies": vs, "include_24hr_change": "true"},
                                timeout=8)
            if resp.status_code == 429:
                try:
                    wait = int(resp.headers.get("Retry-After", "60"))
                except ValueError:
                    wait = 60
                crypto_price._retry_after_until = time.time() + max(30, min(wait, 3600))
                logger.warning("CoinGecko: troppe richieste (429), pausa di %d s", max(30, min(wait, 3600)))
                return {"items": [{"label": "CoinGecko", "value": "in pausa (troppe richieste)"}], "error": True}
            resp.raise_for_status()
            data = resp.json()
        except Exception as exc:
            logger.error("Errore fetch prezzi crypto: %s", exc)
            return {"items": [{"label": "CoinGecko", "value": "non raggiungibile"}], "error": True}

        items = []
        for c in coins:
            label = KNOWN.get(c, c.upper()[:8])
            row = data.get(c)
            if not row or vs not in row:
                items.append({"label": label, "value": "id sconosciuto"})
                continue
            change = row.get(f"{vs}_24h_change")
            value = f"{_fmt_price(row[vs])} {SYMBOL.get(vs, vs.upper())}"
            item = {"label": label, "value": value}
            if isinstance(change, (int, float)):
                item["value"] += f"  {'+' if change >= 0 else ''}{change:.1f}%".replace(".", ",")
                item["color"] = UP if change >= 0 else DOWN
            items.append(item)
        return {"title": "Crypto", "items": items}
