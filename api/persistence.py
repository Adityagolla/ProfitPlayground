"""
persistence.py — DB write helpers driven by engine events.

The event pump (in main.py) drains the engine's event stream and calls
into here to persist:
  - orders snapshot (upsert)
  - order_events (append)
  - trades (insert)
  - portfolio_ledger (debit + credit per fill)

All helpers are no-ops when DB is disabled so the API still works without
Postgres. Errors are logged but never raised — DB hiccups must not break
the live event fan-out to WebSocket clients.
"""

from __future__ import annotations
import logging
import time

from .db import session, is_enabled

log = logging.getLogger("api.persistence")


# ─── Status -> event_type mapping ───────────────────────────────────────────
_STATUS_TO_EVENT = {
    "OPEN":      "ACCEPTED",
    "PARTIAL":   "PARTIAL",
    "FILLED":    "FILLED",
    "CANCELLED": "CANCELLED",
    "REJECTED":  "REJECTED",
}


async def ensure_seed(symbols: list[str], accounts: list[int]) -> None:
    """Seed reference data so FKs from orders/trades resolve.

    Called once at startup. Safe to call repeatedly — uses ON CONFLICT
    DO NOTHING semantics.
    """
    if not is_enabled():
        return
    try:
        from sqlalchemy.dialects.postgresql import insert as pg_insert
        from .models import Symbol, Account

        ts = time.monotonic_ns()
        async with session() as s:
            if s is None:
                return
            await s.execute(
                pg_insert(Symbol).values(
                    [{"symbol": sym} for sym in symbols]
                ).on_conflict_do_nothing(index_elements=["symbol"])
            )
            await s.execute(
                pg_insert(Account).values(
                    [{"account_id": a, "created_ts_ns": ts} for a in accounts]
                ).on_conflict_do_nothing(index_elements=["account_id"])
            )
            await s.commit()
    except Exception as e:  # pragma: no cover
        log.warning("seed failed: %s", e)


async def persist_order_snapshot(order: dict) -> None:
    """Upsert into orders + append a row in order_events."""
    if not is_enabled():
        return
    try:
        from sqlalchemy.dialects.postgresql import insert as pg_insert
        from .models import OrderRow, OrderEvent, OrderIdempotency

        async with session() as s:
            if s is None:
                return

            # Upsert idempotency record first (best-effort).
            await s.execute(
                pg_insert(OrderIdempotency).values(
                    account_id=order["account_id"],
                    client_order_id=order["client_order_id"],
                    server_order_id=order["server_order_id"],
                    created_ts_ns=order["created_ts_ns"],
                ).on_conflict_do_nothing(
                    index_elements=["account_id", "client_order_id"],
                )
            )

            # Upsert order snapshot. Strip keys that aren't columns of `orders`
            # (e.g. `seq`, which we use only for the order_events row).
            order_cols = {c.name for c in OrderRow.__table__.columns}
            order_row = {k: v for k, v in order.items() if k in order_cols}
            stmt = pg_insert(OrderRow).values(**order_row)
            stmt = stmt.on_conflict_do_update(
                index_elements=["server_order_id"],
                set_={
                    "qty_remain":    stmt.excluded.qty_remain,
                    "qty_filled":    stmt.excluded.qty_filled,
                    "status":        stmt.excluded.status,
                    "price":         stmt.excluded.price,
                    "updated_ts_ns": stmt.excluded.updated_ts_ns,
                },
            )
            await s.execute(stmt)

            # Append matching order_events row (one per state seen).
            ev_type = _STATUS_TO_EVENT.get(order["status"], "ACCEPTED")
            s.add(OrderEvent(
                order_id   = order["server_order_id"],
                account_id = order["account_id"],
                symbol     = order["symbol"],
                event_type = ev_type,
                qty_remain = order["qty_remain"],
                qty_filled = order["qty_filled"],
                price      = order["price"],
                seq        = order.get("seq", 0),
                ts_ns      = order["updated_ts_ns"],
            ))
            await s.commit()
    except Exception as e:  # pragma: no cover
        log.warning("persist_order_snapshot failed: %s", e)


async def persist_trade(trade: dict, *, buyer_account: int | None = None,
                        seller_account: int | None = None) -> None:
    """Insert a trade and write balanced ledger entries (debit + credit)."""
    if not is_enabled():
        return
    try:
        from sqlalchemy.dialects.postgresql import insert as pg_insert
        from .models import TradeRow, PortfolioLedger

        notional = trade["price"] * trade["qty"]

        async with session() as s:
            if s is None:
                return

            await s.execute(
                pg_insert(TradeRow).values(**trade)
                .on_conflict_do_nothing(index_elements=["trade_id"])
            )

            # Buyer leg: cash out (debit), qty in (credit).
            if buyer_account is not None:
                s.add_all([
                    PortfolioLedger(
                        account_id=buyer_account, symbol=trade["symbol"],
                        entry_type="fill_debit",
                        qty=0, notional=-notional, price=trade["price"],
                        ref_trade_id=trade["trade_id"], ts_ns=trade["ts_ns"],
                    ),
                    PortfolioLedger(
                        account_id=buyer_account, symbol=trade["symbol"],
                        entry_type="fill_credit",
                        qty=trade["qty"], notional=0, price=trade["price"],
                        ref_trade_id=trade["trade_id"], ts_ns=trade["ts_ns"],
                    ),
                ])

            # Seller leg: qty out (debit), cash in (credit).
            if seller_account is not None:
                s.add_all([
                    PortfolioLedger(
                        account_id=seller_account, symbol=trade["symbol"],
                        entry_type="fill_debit",
                        qty=-trade["qty"], notional=0, price=trade["price"],
                        ref_trade_id=trade["trade_id"], ts_ns=trade["ts_ns"],
                    ),
                    PortfolioLedger(
                        account_id=seller_account, symbol=trade["symbol"],
                        entry_type="fill_credit",
                        qty=0, notional=notional, price=trade["price"],
                        ref_trade_id=trade["trade_id"], ts_ns=trade["ts_ns"],
                    ),
                ])

            await s.commit()
    except Exception as e:  # pragma: no cover
        log.warning("persist_trade failed: %s", e)


async def update_watermark(source: str, last_seq: int) -> None:
    """Persist the last consumed seq for a given event source."""
    if not is_enabled():
        return
    try:
        from sqlalchemy.dialects.postgresql import insert as pg_insert
        from .models import EventWatermark

        async with session() as s:
            if s is None:
                return
            stmt = pg_insert(EventWatermark).values(
                source=source, last_seq=last_seq,
                updated_ts_ns=time.monotonic_ns(),
            ).on_conflict_do_update(
                index_elements=["source"],
                set_={"last_seq": last_seq,
                      "updated_ts_ns": time.monotonic_ns()},
            )
            await s.execute(stmt)
            await s.commit()
    except Exception as e:  # pragma: no cover
        log.warning("update_watermark failed: %s", e)
