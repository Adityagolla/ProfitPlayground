"""
market_maker.py — per-book liquidity bot tied to a live price feed.

One bot task per player book in a face-off room. Every tick it:
  1. fetches the live price for the book's instrument,
  2. cancels its previous two quotes,
  3. re-quotes bid/ask around the live mid through the same engine bridge
     the REST layer uses (so risk, fills, events, and persistence all see
     it as a normal account).

Players trade against these quotes, so their P&L tracks the real market.
The bot is deliberately simple — one level each side, fixed size, a few
basis points wide. It is not trying to win; it is the market.
"""

from __future__ import annotations
import asyncio
import itertools
import logging
import time

import httpx

from .engine_bridge import engine
from .instruments import Instrument, fetch_quote, to_engine_price

log = logging.getLogger("api.market_maker")

BOT_CASH = 10 ** 14          # deep pockets — the bot must never run dry
QUOTE_QTY = 50               # lots per side per refresh
HALF_SPREAD_BPS = 5          # quote at live ± 0.05%

# Distinct client_order_id space per bot account (idempotency key is
# (account_id, client_order_id); bots restart counters per process).
_coid = itertools.count(1_000_000_000)


class BookState:
    """Mutable per-book state shared with the rooms layer for scoring."""

    def __init__(self) -> None:
        self.mark_price: int | None = None   # last live engine price
        self.live_price: float | None = None  # last real-world price
        self.change_pct: float | None = None


async def _cancel_quiet(order_id: int | None) -> None:
    if order_id:
        try:
            await engine.cancel_order(order_id)
        except Exception:
            pass


async def run_market_maker(symbol: str, instr: Instrument, bot_account: int,
                           state: BookState, ends_at: float) -> None:
    """Quote around the live price until the match clock runs out."""
    ok = await engine.ensure_account(symbol, bot_account, cash=BOT_CASH)
    if not ok:
        log.warning("mm_account_failed", extra={"symbol": symbol})
        return

    bid_id: int | None = None
    ask_id: int | None = None

    try:
        async with httpx.AsyncClient(timeout=5.0) as client:
            while time.time() < ends_at:
                quote = await fetch_quote(client, instr)
                if quote:
                    px = to_engine_price(instr, quote["price"])
                    state.mark_price = px
                    state.live_price = quote["price"]
                    state.change_pct = quote.get("change_pct")

                    half = max(1, px * HALF_SPREAD_BPS // 10_000)
                    await _cancel_quiet(bid_id)
                    await _cancel_quiet(ask_id)
                    bid_id = await _quote(symbol, bot_account, "BID", px - half)
                    ask_id = await _quote(symbol, bot_account, "ASK", px + half)
                await asyncio.sleep(instr.poll_s)
    except asyncio.CancelledError:
        pass
    finally:
        # Full time (or room torn down): pull our quotes so the final book
        # state is only what the players left behind.
        await _cancel_quiet(bid_id)
        await _cancel_quiet(ask_id)


async def _quote(symbol: str, account: int, side: str, price: int) -> int | None:
    if price <= 0:
        return None
    ack = await engine.submit_order({
        "action": "NEW",
        "client_order_id": next(_coid),
        "account_id": account,
        "symbol": symbol,
        "type": "LIMIT",
        "side": side,
        "price": price,
        "trigger_price": 0,
        "qty": QUOTE_QTY,
        "ttl_ns": 0,
    })
    return ack["server_order_id"] if ack["status"] == "ACCEPTED" else None
