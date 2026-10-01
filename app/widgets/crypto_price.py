"""
Widget: prezzo di una criptovaluta (default BTC/USDT) da CoinGecko.
Esempio di modulo "unico" configurabile: il simbolo è scelto dalla web UI.
"""
import logging
import time
from typing import Any, Dict, Optional

import requests

from app.widgets.base import Widget, WidgetMeta

logger = logging.getLogger(__name__)

_COINGECKO_URL = "https://api.coingecko.com/api/v3/simple/price"
_retry_after_until = 0.0  # CoinGecko ha chiesto di aspettare fino a questo istante


class CryptoPriceWidget(Widget):
    meta = WidgetMeta(
        id="crypto_price",
        name="Prezzo Crypto",
        description="Prezzo e variazione 24h di una criptovaluta (CoinGecko)",
        icon="trending-up",
        # 60 s: l'API gratuita di CoinGecko limita le richieste per IP (condiviso
        # con gli altri servizi del server). Dopo un "429" si aspetta quanto
        # chiede lei (Retry-After), e intanto il display mostra l'ultimo prezzo.
        refresh_seconds=60,
        layout_type="big_number",
    )

    def get_config_schema(self) -> Dict[str, Any]:
        return {
            "coin_id": {"type": "select", "label": "Moneta", "default": "bitcoin",
                        "options": ["bitcoin", "ethereum", "solana", "ripple", "tether-gold"]},
            "vs_currency": {"type": "select", "label": "Valuta", "default": "usd",
                            "options": ["usd", "eur"]},
        }

    def get_data(self, config: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        config = config or {}
        global _retry_after_until
        coin_id = config.get("coin_id", "bitcoin")
        vs_currency = config.get("vs_currency", "usd")
        if time.time() < _retry_after_until:
            # In pausa su richiesta di CoinGecko: nessuna chiamata (ne allungherebbe il blocco).
            return {"title": coin_id.upper(), "value": None, "unit": vs_currency.upper(), "error": True}
        try:
            resp = requests.get(
                _COINGECKO_URL,
                params={"ids": coin_id, "vs_currencies": vs_currency, "include_24hr_change": "true"},
                timeout=5,
            )
            if resp.status_code == 429:
                try:
                    wait = int(resp.headers.get("Retry-After", "60"))
                except ValueError:
                    wait = 60
                _retry_after_until = time.time() + max(30, min(wait, 3600))
                logger.warning("CoinGecko: troppe richieste (429), pausa di %d s", max(30, min(wait, 3600)))
                return {"title": coin_id.upper(), "value": None, "unit": vs_currency.upper(), "error": True}
            resp.raise_for_status()
            data = resp.json()[coin_id]
            return {
                "title": coin_id.upper(),
                "value": round(data[vs_currency], 2),
                "unit": vs_currency.upper(),
                "change_24h": round(data.get(f"{vs_currency}_24h_change", 0), 2),
            }
        except Exception as exc:
            logger.error("Errore fetch prezzo crypto: %s", exc)
            return {"title": coin_id.upper(), "value": None, "unit": vs_currency.upper(), "error": True}

    def health_check(self) -> bool:
        try:
            requests.get(_COINGECKO_URL, params={"ids": "bitcoin", "vs_currencies": "usd"}, timeout=3)
            return True
        except Exception:
            return False
