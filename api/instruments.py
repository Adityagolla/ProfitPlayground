"""
instruments.py — the tradable roster + live price feeds.

Face-off mode: each player picks one instrument from this roster (like
picking a team in FIFA) and trades it in their own book against a
market-maker bot that quotes around the live price.

Feeds:
  - binance  — public REST, no API key, 24/7 (crypto pairs)
  - finnhub  — free API key required (FINNHUB_API_KEY), US market hours

Price convention: the engine speaks integer fixed-point with a scale of
100 (i.e. cents). Crypto trades in sub-unit lots so one lot stays in a
game-friendly price range: engine_price = round(real_price / divisor * 100)
and one engine qty unit = (1 / divisor) of the real asset.
"""

from __future__ import annotations
import asyncio
import time
from dataclasses import dataclass
from typing import Optional

import httpx

from .config import settings

PRICE_SCALE = 100          # engine ints are hundredths (cents)
QUOTE_CACHE_TTL_S = 10.0   # roster-screen quote cache


@dataclass(frozen=True)
class Instrument:
    id: str            # roster key, e.g. "BTCUSDT" / "NVDA" (<= 7 chars)
    display: str       # "BTC/USDT"
    name: str          # "Bitcoin"
    feed: str          # "binance" | "finnhub"
    feed_symbol: str   # symbol in the feed's namespace
    lot_label: str     # what one engine qty unit represents
    divisor: int       # real price per unit -> per lot (1 for stocks)
    poll_s: float      # market-maker refresh interval


# Curated roster. Crypto is always available (keyless). Stocks are the
# 2025 top performers + a couple of banks; shown only when a Finnhub key
# is configured. Instrument ids must stay <= 7 chars so the room-scoped
# engine symbol ("<code><player>:<id>") fits OB_MAX_SYMBOL_LEN.
_CRYPTO: list[Instrument] = [
    Instrument("BTCUSDT", "BTC/USDT", "Bitcoin",  "binance", "BTCUSDT",
               "0.001 BTC", 1000, 1.5),
    Instrument("ETHUSDT", "ETH/USDT", "Ethereum", "binance", "ETHUSDT",
               "0.01 ETH", 100, 1.5),
    Instrument("SOLUSDT", "SOL/USDT", "Solana",   "binance", "SOLUSDT",
               "0.1 SOL", 10, 1.5),
]

_STOCKS: list[Instrument] = [
    Instrument("NVDA", "NVDA", "NVIDIA",           "finnhub", "NVDA", "1 share", 1, 5.0),
    Instrument("PLTR", "PLTR", "Palantir",         "finnhub", "PLTR", "1 share", 1, 5.0),
    Instrument("TSLA", "TSLA", "Tesla",            "finnhub", "TSLA", "1 share", 1, 5.0),
    Instrument("META", "META", "Meta Platforms",   "finnhub", "META", "1 share", 1, 5.0),
    Instrument("AVGO", "AVGO", "Broadcom",         "finnhub", "AVGO", "1 share", 1, 5.0),
    Instrument("JPM",  "JPM",  "JPMorgan Chase",   "finnhub", "JPM",  "1 share", 1, 5.0),
    Instrument("GS",   "GS",   "Goldman Sachs",    "finnhub", "GS",   "1 share", 1, 5.0),
]


def available() -> list[Instrument]:
    out = list(_CRYPTO)
    if settings.finnhub_api_key:
        out.extend(_STOCKS)
    return out


def by_id(instrument_id: str) -> Optional[Instrument]:
    for i in available():
        if i.id == instrument_id:
            return i
    return None


# ─── Live quotes ─────────────────────────────────────────────────────────────

async def fetch_quote(client: httpx.AsyncClient,
                      instr: Instrument) -> Optional[dict]:
    """One live quote: {"price": float, "change_pct": float | None}.

    Returns None on any feed hiccup — callers (bot / roster) skip the tick
    rather than fail. Finnhub returns c=0 outside market hours for some
    symbols; treat that as unavailable.
    """
    try:
        if instr.feed == "binance":
            r = await client.get(
                "https://api.binance.com/api/v3/ticker/24hr",
                params={"symbol": instr.feed_symbol})
            r.raise_for_status()
            d = r.json()
            price = float(d["lastPrice"])
            change = float(d["priceChangePercent"])
            return {"price": price, "change_pct": change} if price > 0 else None

        if instr.feed == "finnhub":
            r = await client.get(
                "https://finnhub.io/api/v1/quote",
                params={"symbol": instr.feed_symbol,
                        "token": settings.finnhub_api_key})
            r.raise_for_status()
            d = r.json()
            price = float(d.get("c") or 0)
            change = d.get("dp")
            if price <= 0:
                return None
            return {"price": price,
                    "change_pct": float(change) if change is not None else None}
    except Exception:
        return None
    return None


def to_engine_price(instr: Instrument, real_price: float) -> int:
    """Real-world price -> integer engine price for one lot."""
    return max(1, round(real_price / instr.divisor * PRICE_SCALE))


# Small TTL cache so the roster screen doesn't hammer the feeds while
# both players stare at the pick list.
_quote_cache: dict[str, tuple[float, Optional[dict]]] = {}
_cache_lock = asyncio.Lock()


async def cached_quotes(instrs: list[Instrument]) -> dict[str, Optional[dict]]:
    now = time.monotonic()
    async with _cache_lock:
        missing = [i for i in instrs
                   if i.id not in _quote_cache
                   or now - _quote_cache[i.id][0] > QUOTE_CACHE_TTL_S]
        if missing:
            async with httpx.AsyncClient(timeout=5.0) as client:
                results = await asyncio.gather(
                    *(fetch_quote(client, i) for i in missing))
            for instr, q in zip(missing, results):
                _quote_cache[instr.id] = (now, q)
        return {i.id: _quote_cache.get(i.id, (0, None))[1] for i in instrs}
