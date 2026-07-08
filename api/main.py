"""
main.py — FastAPI entry point.

Wires:
  - CORS
  - A global error envelope
  - REST routers (orders, orderbook, trades, portfolio)
  - WebSocket router (/ws)
  - The event pump: a background task that drains engine_bridge.engine.events()
    and broadcasts to WS subscribers.

Run locally:
    python -m api.run
or:
    uvicorn api.main:app --reload --port 8080
"""

from __future__ import annotations
import asyncio

from contextlib import asynccontextmanager
from fastapi import FastAPI, HTTPException, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from sqlalchemy import text as _sql_text

from .config import settings
from . import db, persistence
from .engine_bridge import engine
from .subscriptions import manager
from .observability import (
    setup_logging, log, RequestIdMiddleware, instrument_app,
    PUMP_LAG, PERSIST_FAILURES,
)
from .routes_orders    import router as orders_router
from .routes_market    import router_book, router_trades
from .routes_portfolio import router as portfolio_router
from .routes_ws        import router as ws_router
from .rooms            import router as rooms_router, registry as rooms_registry

# Reference data we seed at startup so FKs resolve immediately.
SEED_SYMBOLS  = ["AAPL", "MSFT", "GOOG"]
SEED_ACCOUNTS = [1, 2, 3]

# Initialise structured logging before anything else logs.
setup_logging(level="DEBUG" if settings.dev else "INFO",
              json_output=not settings.dev)


# ─── Event pump ──────────────────────────────────────────────────────────────
async def _persist(msg: dict) -> None:
    """Write the appropriate rows for one engine event.

    Wrapped in try/except so DB failures never break WS broadcasts.
    Each failure increments PERSIST_FAILURES so we can alert on it.
    """
    ev = msg.get("event")
    data = msg.get("data", {})
    try:
        if ev == "order":
            data_with_seq = {**data, "seq": msg.get("seq", 0)}
            await persistence.persist_order_snapshot(data_with_seq)
        elif ev == "trade":
            buyer_acct = seller_acct = None
            agg = await engine.get_order(data.get("aggressor_id", 0))
            rest = await engine.get_order(data.get("resting_id", 0))
            agg_side = data.get("aggressor_side")
            if agg and agg_side == "BID":
                buyer_acct = agg.get("account_id")
                if rest:
                    seller_acct = rest.get("account_id")
            elif agg and agg_side == "ASK":
                seller_acct = agg.get("account_id")
                if rest:
                    buyer_acct = rest.get("account_id")
            await persistence.persist_trade(
                data, buyer_account=buyer_acct, seller_account=seller_acct,
            )
    except Exception as e:  # pragma: no cover
        PERSIST_FAILURES.labels(stage=ev or "unknown").inc()
        log.warning("persist_failed", event=ev, error=str(e))

    if "seq" in msg:
        try:
            await persistence.update_watermark("engine_bus_main", msg["seq"])
        except Exception as e:  # pragma: no cover
            PERSIST_FAILURES.labels(stage="watermark").inc()
            log.warning("watermark_failed", error=str(e))
        finally:
            # Whether or not the watermark write succeeded, this event is no
            # longer in flight as far as the pump is concerned.
            PUMP_LAG.dec()


async def _pump_events() -> None:
    """Drain engine events: persist (best-effort) then fan-out to WS clients."""
    async for msg in engine.events():
        if msg.get("seq") is not None:
            # Inc on receipt, dec after _persist (in the finally above). The
            # gauge value == events currently being persisted/broadcast.
            PUMP_LAG.inc()
        await _persist(msg)
        await manager.broadcast(msg)


# ─── Lifespan ────────────────────────────────────────────────────────────────
@asynccontextmanager
async def lifespan(app: FastAPI):
    # Bring up DB (no-op if DATABASE_URL is empty), seed reference rows.
    await db.init()
    await persistence.ensure_seed(SEED_SYMBOLS, SEED_ACCOUNTS)

    task = asyncio.create_task(_pump_events(), name="engine-event-pump")
    log.info(
        "api_started",
        host=settings.host, port=settings.port,
        dev=settings.dev, db=db.is_enabled(),
    )
    try:
        yield
    finally:
        await rooms_registry.shutdown()   # cancel market-maker bots
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass
        await db.shutdown()


app = FastAPI(
    title="Trade Pipeline API",
    version="0.1.0",
    lifespan=lifespan,
)

# Order matters: request-id first (so subsequent middleware can read it),
# then CORS, then prometheus instrumentation (registered separately below).
app.add_middleware(RequestIdMiddleware)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],  # tighten in production
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Mount /metrics + default HTTP latency/throughput histograms.
instrument_app(app)


# ─── Global error envelope ──────────────────────────────────────────────────
@app.exception_handler(HTTPException)
async def _http_exc_handler(request: Request, exc: HTTPException):
    """Keep every error response shaped as {error:{code,message,...}}."""
    body = exc.detail
    if isinstance(body, dict) and "error" in body:
        return JSONResponse(status_code=exc.status_code, content=body)
    return JSONResponse(
        status_code=exc.status_code,
        content={"error": {"code": f"HTTP_{exc.status_code}",
                           "message": str(exc.detail)}},
    )


# ─── Routers ────────────────────────────────────────────────────────────────
app.include_router(orders_router)
app.include_router(router_book)
app.include_router(router_trades)
app.include_router(portfolio_router)
app.include_router(ws_router)
app.include_router(rooms_router)


@app.get("/health", tags=["meta"])
async def health() -> dict:
    """Trivial liveness probe. Always returns OK."""
    return {"status": "ok"}


@app.get("/ready", tags=["meta"])
async def ready() -> JSONResponse:
    """Readiness probe — green only when DB is reachable (or memory-only)."""
    if not db.is_enabled():
        return JSONResponse({"status": "ok", "db": "disabled"})
    try:
        async with db.session() as s:
            await s.execute(_sql_text("select 1"))
        return JSONResponse({"status": "ok", "db": "up"})
    except Exception as e:
        return JSONResponse(
            {"status": "degraded", "db": "down", "error": str(e)},
            status_code=503,
        )
