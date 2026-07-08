"""
schemas.py — Pydantic request/response models.

Field naming: snake_case everywhere (matches the C structs).
Timestamps: nanoseconds since a monotonic epoch, as integers (ts_ns).
Prices / qty: integer fixed-point to stay bit-exact with the C engine.
             Caller is responsible for knowing the price scale of each symbol.
"""

from __future__ import annotations
from typing import Literal, Optional
from pydantic import BaseModel, Field


# ─── Orders ──────────────────────────────────────────────────────────────────

OrderType = Literal["LIMIT", "MARKET", "IOC", "FOK", "STOP", "STOP_LIMIT"]
OrderSide = Literal["BID", "ASK"]
OrderAction = Literal["NEW", "CANCEL", "MODIFY"]


class NewOrderRequest(BaseModel):
    """Body for POST /orders. Mirrors gw_raw_msg_t in gateway.h."""
    client_order_id: int = Field(..., gt=0, description="Client-side dedup key; same id = duplicate")
    account_id: int = Field(..., gt=0)
    symbol: str = Field(..., min_length=1, max_length=16)
    type: OrderType
    side: OrderSide
    price: int = Field(0, ge=0, description="Fixed-point price; 0 for MARKET")
    trigger_price: int = Field(0, ge=0, description="Stops only")
    qty: int = Field(..., gt=0)
    ttl_ns: int = Field(0, ge=0, description="0 = GTC")


class PatchOrderRequest(BaseModel):
    """Body for PATCH /orders/{id}. Server behaves like cancel + re-add."""
    new_price: Optional[int] = Field(None, ge=0)
    new_qty: Optional[int] = Field(None, gt=0)


class OrderAck(BaseModel):
    """Mirrors gw_ack_t. This is what every /orders mutation returns."""
    status: Literal["ACCEPTED", "REJECTED", "DUPLICATE", "THROTTLED", "KILLED"]
    server_order_id: int
    client_order_id: int
    ingress_ts_ns: int
    ack_ts_ns: int
    reject_reason: Optional[str] = None


class OrderView(BaseModel):
    """Returned by GET /orders/{id}."""
    server_order_id: int
    client_order_id: int
    account_id: int
    symbol: str
    type: OrderType
    side: OrderSide
    price: int
    trigger_price: int
    qty_original: int
    qty_remain: int
    qty_filled: int
    status: Literal["OPEN", "FILLED", "PARTIAL", "CANCELLED", "REJECTED"]
    created_ts_ns: int
    updated_ts_ns: int


class CancelResponse(BaseModel):
    """DELETE /orders/{id} response."""
    status: Literal["cancelled", "not_found"]
    order_id: int
    cancelled_qty: int = 0


# ─── Order Book ──────────────────────────────────────────────────────────────

class Level(BaseModel):
    """Price ladder row."""
    price: int
    qty: int


class TopOfBook(BaseModel):
    symbol: str
    bid_price: int
    bid_qty: int
    ask_price: int
    ask_qty: int
    spread: int
    mid: int
    ts_ns: int


class BookSnapshot(BaseModel):
    symbol: str
    bids: list[Level]
    asks: list[Level]
    seq: int
    ts_ns: int


# ─── Trades ──────────────────────────────────────────────────────────────────

class Trade(BaseModel):
    trade_id: int
    symbol: str
    price: int
    qty: int
    aggressor_side: OrderSide
    aggressor_id: int
    resting_id: int
    ts_ns: int


class TradesPage(BaseModel):
    """Cursor-paged response. `next_cursor` is opaque; clients echo it back."""
    items: list[Trade]
    next_cursor: Optional[str] = None


# ─── Portfolio ───────────────────────────────────────────────────────────────

class Position(BaseModel):
    symbol: str
    net_qty: int
    avg_price: int
    unrealised_pnl: int


class PortfolioView(BaseModel):
    user_id: int
    cash: int
    positions: list[Position]
    realised_pnl: int
    open_buy_notional: int
    open_sell_qty: int


# ─── Errors ──────────────────────────────────────────────────────────────────

class ErrorBody(BaseModel):
    code: str
    message: str
    retry_after_ms: Optional[int] = None


class ErrorEnvelope(BaseModel):
    error: ErrorBody
