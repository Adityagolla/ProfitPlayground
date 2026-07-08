"""
routes_market.py — Public read-only market data routes.

These are the equivalent of the C pipeline's BOOK / TRADE events, but
served synchronously over REST (good for browsers and thin clients that
don't want to hold a WS open).

No auth on purpose — the book and tape are public.
"""

from fastapi import APIRouter, Query

from .engine_bridge import engine
from .schemas import BookSnapshot, TopOfBook, TradesPage, Trade

router_book   = APIRouter(prefix="/orderbook", tags=["orderbook"])
router_trades = APIRouter(prefix="/trades",    tags=["trades"])


@router_book.get("/{symbol}", response_model=BookSnapshot)
async def get_book(symbol: str, depth: int = Query(20, ge=1, le=200)) -> BookSnapshot:
    """Full ladder snapshot up to `depth` levels per side."""
    return BookSnapshot(**(await engine.book_snapshot(symbol, depth)))


@router_book.get("/{symbol}/top", response_model=TopOfBook)
async def get_top(symbol: str) -> TopOfBook:
    """Cheapest possible shape: best bid + best ask + spread + mid."""
    return TopOfBook(**(await engine.top_of_book(symbol)))


@router_book.get("/{symbol}/depth", response_model=BookSnapshot)
async def get_depth(symbol: str, levels: int = Query(10, ge=1, le=200)) -> BookSnapshot:
    """Alias for the snapshot endpoint with a client-capped level count."""
    return BookSnapshot(**(await engine.book_snapshot(symbol, levels)))


@router_trades.get("", response_model=TradesPage)
async def get_trades(symbol: str, limit: int = Query(50, ge=1, le=500),
                     cursor: str | None = None) -> TradesPage:
    """Cursor-paged tape. `next_cursor` is an opaque string — echo it back."""
    items, nxt = await engine.trades(symbol, cursor, limit)
    return TradesPage(items=[Trade(**t) for t in items], next_cursor=nxt)
