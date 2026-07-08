"""
models.py — SQLAlchemy schema for the trade pipeline.

Design intent
─────────────
- `orders` is a *snapshot* of the latest state per order (fast reads).
- `order_events` is *append-only* (audit + replay source of truth — your
  `me_event_t` persisted).
- `trades` is *append-only* (one row per fill out of the engine).
- `portfolio_ledger` is *append-only* (every cash/qty mutation).
  Positions and cash are computed by summing ledger entries; this gives
  the player a full transaction history for free.
- `order_idempotency` survives restarts so dedup still works after a crash.
- `symbols` is reference data for risk parameters.
- `accounts` is the engine-level account; `users` is left out for later.
- `event_watermarks` lets the event pump resume from a known sequence.

All money/qty values are integers (fixed-point). All timestamps are int64
nanoseconds from a monotonic clock.
"""

from __future__ import annotations

try:
    from sqlalchemy import (
        BigInteger, Boolean, CheckConstraint, Column, ForeignKey,
        Index, Integer, SmallInteger, String, UniqueConstraint,
    )
    from sqlalchemy.orm import DeclarativeBase
    _SA_OK = True
except Exception:  # pragma: no cover
    _SA_OK = False
    class DeclarativeBase:  # type: ignore
        pass


class Base(DeclarativeBase):
    """Single declarative base shared by every table."""
    pass


if _SA_OK:

    # ─── Reference data ──────────────────────────────────────────────────

    class Symbol(Base):
        """Tradable instruments + risk parameters."""
        __tablename__ = "symbols"
        symbol         = Column(String(16), primary_key=True)
        price_scale    = Column(BigInteger,  nullable=False, default=100)   # 1e2 = cents
        tick_size      = Column(BigInteger,  nullable=False, default=1)
        fat_finger_bps = Column(Integer,     nullable=False, default=1000)  # 10%
        is_active      = Column(Boolean,     nullable=False, default=True)


    class Account(Base):
        """Engine-side account. user_id is reserved for future auth."""
        __tablename__ = "accounts"
        account_id   = Column(Integer,    primary_key=True)
        user_id      = Column(BigInteger, nullable=True)
        status       = Column(String(10), nullable=False, default="ACTIVE")
        created_ts_ns = Column(BigInteger, nullable=False, default=0)


    # ─── Order state ─────────────────────────────────────────────────────

    class OrderRow(Base):
        """Latest snapshot per server_order_id."""
        __tablename__ = "orders"
        server_order_id = Column(BigInteger, primary_key=True)
        client_order_id = Column(BigInteger, nullable=False)
        account_id      = Column(Integer,
                                 ForeignKey("accounts.account_id"),
                                 nullable=False)
        symbol          = Column(String(16),
                                 ForeignKey("symbols.symbol"),
                                 nullable=False)
        type            = Column(String(12), nullable=False)
        side            = Column(String(3),  nullable=False)
        price           = Column(BigInteger, nullable=False)
        trigger_price   = Column(BigInteger, nullable=False, default=0)
        qty_original    = Column(BigInteger, nullable=False)
        qty_remain      = Column(BigInteger, nullable=False)
        qty_filled      = Column(BigInteger, nullable=False, default=0)
        status          = Column(String(10), nullable=False)
        created_ts_ns   = Column(BigInteger, nullable=False)
        updated_ts_ns   = Column(BigInteger, nullable=False)

        __table_args__ = (
            Index("ix_orders_account", "account_id"),
            Index("ix_orders_symbol",  "symbol"),
            Index("ix_orders_symbol_status", "symbol", "status"),
        )


    class OrderIdempotency(Base):
        """Survives restarts so duplicate POSTs still return DUPLICATE."""
        __tablename__ = "order_idempotency"
        # Composite primary key on the natural dedup pair.
        account_id      = Column(Integer,    primary_key=True)
        client_order_id = Column(BigInteger, primary_key=True)
        server_order_id = Column(BigInteger, nullable=False)
        created_ts_ns   = Column(BigInteger, nullable=False)


    # ─── Append-only history ─────────────────────────────────────────────

    class OrderEvent(Base):
        """One row per state transition (`me_event_t`-equivalent)."""
        __tablename__ = "order_events"
        event_id   = Column(BigInteger, primary_key=True, autoincrement=True)
        order_id   = Column(BigInteger,
                            ForeignKey("orders.server_order_id"),
                            nullable=False)
        account_id = Column(Integer,    nullable=False)
        symbol     = Column(String(16), nullable=False)
        event_type = Column(String(20), nullable=False)
        # Snapshots of qty fields right after the event, for cheap auditing.
        qty_remain = Column(BigInteger, nullable=False, default=0)
        qty_filled = Column(BigInteger, nullable=False, default=0)
        price      = Column(BigInteger, nullable=False, default=0)
        seq        = Column(BigInteger, nullable=False)
        ts_ns      = Column(BigInteger, nullable=False)

        __table_args__ = (
            Index("ix_order_events_order_seq", "order_id", "seq"),
            Index("ix_order_events_symbol_ts", "symbol", "ts_ns"),
            CheckConstraint(
                "event_type IN ('ACCEPTED','PARTIAL','FILLED','CANCELLED',"
                "'REJECTED','MODIFIED','STOP_TRIGGERED','EXPIRED')",
                name="ck_order_events_type",
            ),
        )


    class TradeRow(Base):
        """One row per match (fill)."""
        __tablename__ = "trades"
        trade_id       = Column(BigInteger, primary_key=True)
        symbol         = Column(String(16),
                                ForeignKey("symbols.symbol"),
                                nullable=False)
        price          = Column(BigInteger, nullable=False)
        qty            = Column(BigInteger, nullable=False)
        aggressor_side = Column(String(3),  nullable=False)
        aggressor_id   = Column(BigInteger, nullable=False)
        resting_id     = Column(BigInteger, nullable=False)
        ts_ns          = Column(BigInteger, nullable=False)

        __table_args__ = (
            Index("ix_trades_symbol_ts", "symbol", "ts_ns"),
            Index("ix_trades_aggressor", "aggressor_id"),
            Index("ix_trades_resting",   "resting_id"),
        )


    # ─── Portfolio ledger (player-visible history) ───────────────────────

    class PortfolioLedger(Base):
        """Append-only money/qty movements per account.

        entry_type vocabulary:
          - 'reserve'      : risk reservation on order accept (cash or qty)
          - 'release'      : reservation released on cancel/expire
          - 'fill_debit'   : leg of a fill that *removes* (cash for buy / qty for sell)
          - 'fill_credit'  : leg of a fill that *adds*    (qty for buy / cash for sell)
          - 'fee'          : commission / fee
          - 'deposit'      : external cash in
          - 'withdrawal'   : external cash out
          - 'pnl_realised' : marker entry when a sell closes a position

        For each fill we write *two* rows (debit + credit) so the ledger
        balances if you sum it.
        """
        __tablename__ = "portfolio_ledger"
        ledger_id    = Column(BigInteger, primary_key=True, autoincrement=True)
        account_id   = Column(Integer,    nullable=False)
        symbol       = Column(String(16), nullable=True)   # null for cash-only entries
        entry_type   = Column(String(20), nullable=False)
        qty          = Column(BigInteger, nullable=False, default=0)
        notional     = Column(BigInteger, nullable=False, default=0)  # +ve = credit, -ve = debit
        price        = Column(BigInteger, nullable=False, default=0)
        ref_order_id = Column(BigInteger, nullable=True)
        ref_trade_id = Column(BigInteger, nullable=True)
        ts_ns        = Column(BigInteger, nullable=False)

        __table_args__ = (
            Index("ix_ledger_account_ts", "account_id", "ts_ns"),
            Index("ix_ledger_account_symbol", "account_id", "symbol"),
            CheckConstraint(
                "entry_type IN ('reserve','release','fill_debit','fill_credit',"
                "'fee','deposit','withdrawal','pnl_realised')",
                name="ck_ledger_type",
            ),
        )


    # ─── Pump bookkeeping ────────────────────────────────────────────────

    class EventWatermark(Base):
        """Tracks the last `seq` consumed from a given event source."""
        __tablename__ = "event_watermarks"
        source         = Column(String(64),  primary_key=True)
        last_seq       = Column(BigInteger,  nullable=False, default=0)
        updated_ts_ns  = Column(BigInteger,  nullable=False, default=0)

