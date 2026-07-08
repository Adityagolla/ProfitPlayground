"""
engine_bridge.py — Pluggable bridge between FastAPI and the C pipeline.

Two implementations behind one interface:

  _CEngine  — the real thing. Loads bridge/libpipeline.dll (built with
              `make dll` from bridge/bridge.c + pipeline/ + engine/) via
              ctypes and drives one C pipeline per symbol.
  _Engine   — the original in-memory mock, kept as a fallback so the API
              still runs before the DLL is built (or with PIPELINE_DLL=).

Selection happens once at import: settings.pipeline_dll pointing at an
existing file => _CEngine, otherwise _Engine.

Interface (kept tiny on purpose):
  submit_order(msg)          -> ack dict         (matches gw_ack_t)
  cancel_order(order_id)     -> (status, qty)
  modify_order(order_id, …)  -> order dict | None
  get_order(order_id)        -> order dict | None
  top_of_book(symbol)        -> dict
  book_snapshot(symbol, depth)
  trades(symbol, cursor, lim)-> (items, next_cursor)
  portfolio(user_id)         -> dict
  events()                   -> async generator of bus_msg dicts

Division of labour with the C side (see bridge/bridge.h): the C engine is
the source of truth for matching, order state, and per-symbol books. The
underlying portfolio_t is single-symbol by design, so cross-symbol cash /
PnL aggregation and the trade tape live here in Python, fed from the
bridge's event stream.
"""

from __future__ import annotations
import asyncio
import ctypes
import logging
import os
import time
from collections import defaultdict, deque
from ctypes import (
    POINTER, byref, c_bool, c_char_p, c_int32, c_int64,
    c_uint32, c_uint64, c_void_p,
)
from dataclasses import dataclass, field
from typing import AsyncIterator, Optional

from .config import settings

log = logging.getLogger("api.engine_bridge")


def _now_ns() -> int:
    """Monotonic nanosecond clock — same intent as gw_now_ns in C."""
    return time.monotonic_ns()


# ═══════════════════════════════════════════════════════════════════════════
# ctypes mirrors of bridge/bridge.h — keep field-for-field in sync.
# ═══════════════════════════════════════════════════════════════════════════

class _CRawMsg(ctypes.Structure):
    """bridge_raw_msg_t"""
    _fields_ = [
        ("action",          c_int32),
        ("client_order_id", c_uint64),
        ("account_id",      c_uint32),
        ("type",            c_int32),
        ("side",            c_int32),
        ("price",           c_int64),
        ("trigger_price",   c_int64),
        ("qty",             c_uint64),
        ("ttl_ns",          c_uint64),
        ("target_order_id", c_uint64),
        ("new_price",       c_int64),
        ("new_qty",         c_uint64),
    ]


class _CAck(ctypes.Structure):
    """bridge_ack_t"""
    _fields_ = [
        ("status",          c_int32),
        ("server_order_id", c_uint64),
        ("ingress_ts_ns",   c_uint64),
        ("ack_ts_ns",       c_uint64),
        ("reject_reason",   c_char_p),   # NULL or static string — never freed
    ]


class _COrderView(ctypes.Structure):
    """bridge_order_view_t"""
    _fields_ = [
        ("server_order_id", c_uint64),
        ("client_order_id", c_uint64),
        ("account_id",      c_uint32),
        ("type",            c_int32),
        ("side",            c_int32),
        ("price",           c_int64),
        ("trigger_price",   c_int64),
        ("qty_original",    c_uint64),
        ("qty_remain",      c_uint64),
        ("qty_filled",      c_uint64),
        ("status",          c_int32),
        ("created_ts_ns",   c_uint64),
        ("updated_ts_ns",   c_uint64),
        ("found",           c_bool),
    ]


class _CTob(ctypes.Structure):
    """bridge_tob_t"""
    _fields_ = [
        ("bid_price", c_int64), ("bid_qty", c_int64),
        ("ask_price", c_int64), ("ask_qty", c_int64),
        ("spread",    c_int64), ("mid",     c_int64),
    ]


class _CEvent(ctypes.Structure):
    """bridge_event_t"""
    _fields_ = [
        ("kind",  c_int32),
        ("seq",   c_uint64),
        ("ts_ns", c_uint64),
        # kind == ORDER
        ("order_id",     c_uint64),
        ("account_id",   c_uint32),
        ("side",         c_int32),
        ("type",         c_int32),
        ("price",        c_int64),
        ("qty_original", c_uint64),
        ("qty_remain",   c_uint64),
        ("qty_filled",   c_uint64),
        ("status",       c_int32),
        # kind == TRADE
        ("trade_id",             c_uint64),
        ("aggressor_id",         c_uint64),
        ("resting_id",           c_uint64),
        ("aggressor_account_id", c_uint32),
        ("resting_account_id",   c_uint32),
        ("aggressor_side",       c_int32),
        ("trade_price",          c_int64),
        ("trade_qty",            c_uint64),
    ]


_EVENT_ORDER, _EVENT_TRADE = 0, 1

# Enum value maps (bridge.h / gateway.h / matching_engine.h / orderbook.h)
_TYPE_TO_INT = {"LIMIT": 0, "MARKET": 1, "IOC": 2, "FOK": 3,
                "STOP": 4, "STOP_LIMIT": 5}
_INT_TO_TYPE = {v: k for k, v in _TYPE_TO_INT.items()}
_SIDE_TO_INT = {"BID": 0, "ASK": 1}
_INT_TO_SIDE = {v: k for k, v in _SIDE_TO_INT.items()}
_ACTION_TO_INT = {"NEW": 1, "CANCEL": 2, "MODIFY": 3}
_ACK_STATUS = {0: "ACCEPTED", 1: "REJECTED", 2: "DUPLICATE",
               3: "THROTTLED", 4: "KILLED"}
_ORDER_STATUS = {0: "OPEN", 1: "PARTIAL", 2: "FILLED",
                 3: "CANCELLED", 4: "REJECTED"}
_TERMINAL = {"FILLED", "CANCELLED", "REJECTED"}

# Trade ids are per-pipeline in C (each symbol starts at 1); namespace them
# the same way bridge.c namespaces order ids so the DB PK stays unique.
_TRADE_ID_SPACE = 1_000_000

# Lazily-registered account defaults. Cash matches the mock/demo; 0 for the
# limits means "unchecked" in portfolio.c.
_DEFAULT_CASH = 10_000_000
_DEFAULT_MAX_POSITION = 0
_DEFAULT_MAX_NOTIONAL = 0


# ─── Python-side portfolio aggregation (shared by both engines) ─────────────

@dataclass
class _Position:
    net_qty: int = 0
    avg_price: int = 0
    unrealised_pnl: int = 0


@dataclass
class _Portfolio:
    user_id: int
    cash: int = _DEFAULT_CASH
    realised_pnl: int = 0
    open_buy_notional: int = 0
    open_sell_qty: int = 0
    positions: dict[str, _Position] = field(default_factory=dict)


# ═══════════════════════════════════════════════════════════════════════════
# Real engine — ctypes over bridge/libpipeline.dll
# ═══════════════════════════════════════════════════════════════════════════

class _CEngine:
    """
    Drives the C pipeline through the flat bridge API. One pipeline (order
    book + gateway + risk + portfolio) per symbol, created on first touch.

    Concurrency: the C side is single-threaded and not reentrant, so every
    DLL call happens under self._lock. Calls are non-blocking and micro-
    second-fast, so holding the asyncio lock across them is fine.
    """

    def __init__(self, dll_path: str) -> None:
        self._dll = ctypes.CDLL(dll_path)
        self._declare_prototypes()

        self._lock = asyncio.Lock()
        self._handles: dict[str, int] = {}          # symbol -> bridge handle
        self._symbol_index: dict[str, int] = {}     # symbol -> creation index
        self._accounts_added: set[tuple[str, int]] = set()
        self._order_symbol: dict[int, str] = {}     # server_order_id -> symbol
        self._trades: dict[str, deque] = defaultdict(lambda: deque(maxlen=10_000))
        self._portfolios: dict[int, _Portfolio] = {}
        self._last_price: dict[str, int] = {}
        self._event_q: asyncio.Queue = asyncio.Queue(maxsize=10_000)
        self._next_seq = 0

    def _declare_prototypes(self) -> None:
        d = self._dll
        d.bridge_add_symbol.argtypes = [c_char_p]
        d.bridge_add_symbol.restype = c_void_p
        d.bridge_add_account.argtypes = [c_void_p, c_uint32, c_int64, c_int64, c_int64]
        d.bridge_add_account.restype = c_bool
        d.bridge_submit.argtypes = [c_void_p, POINTER(_CRawMsg)]
        d.bridge_submit.restype = _CAck
        d.bridge_cancel.argtypes = [c_void_p, c_uint64, c_uint32]
        d.bridge_cancel.restype = _CAck
        d.bridge_get_order.argtypes = [c_void_p, c_uint64]
        d.bridge_get_order.restype = _COrderView
        d.bridge_top_of_book.argtypes = [c_void_p]
        d.bridge_top_of_book.restype = _CTob
        d.bridge_book_levels.argtypes = [c_void_p, c_int32, c_uint32,
                                         POINTER(c_int64), POINTER(c_int64)]
        d.bridge_book_levels.restype = c_uint32
        d.bridge_poll_event.argtypes = [c_void_p, POINTER(_CEvent)]
        d.bridge_poll_event.restype = c_bool

    # ── helpers (call with lock held) ─────────────────────────────────────

    def _seq(self) -> int:
        self._next_seq += 1
        return self._next_seq

    def _emit(self, msg: dict) -> None:
        """Push an event onto the fan-out queue (drop-oldest on overflow)."""
        try:
            self._event_q.put_nowait(msg)
        except asyncio.QueueFull:
            try:
                _ = self._event_q.get_nowait()
                self._event_q.put_nowait(msg)
            except Exception:
                pass

    def _handle(self, symbol: str) -> Optional[int]:
        h = self._handles.get(symbol)
        if h is not None:
            return h
        h = self._dll.bridge_add_symbol(symbol.encode("ascii", "replace"))
        if not h:
            return None                       # table full / name too long
        self._symbol_index[symbol] = len(self._handles)
        self._handles[symbol] = h
        return h

    def _ensure_account(self, symbol: str, h: int, account_id: int) -> None:
        key = (symbol, account_id)
        if key in self._accounts_added:
            return
        ok = self._dll.bridge_add_account(
            h, account_id, _DEFAULT_CASH,
            _DEFAULT_MAX_POSITION, _DEFAULT_MAX_NOTIONAL)
        if ok:
            self._accounts_added.add(key)

    def _portfolio_for(self, user_id: int) -> _Portfolio:
        p = self._portfolios.get(user_id)
        if p is None:
            p = _Portfolio(user_id=user_id)
            self._portfolios[user_id] = p
        return p

    def _apply_fill(self, account_id: int, symbol: str, is_buy: bool,
                    price: int, qty: int) -> None:
        """Average-cost position/cash accounting for one fill leg."""
        if account_id == 0:
            return
        p = self._portfolio_for(account_id)
        pos = p.positions.get(symbol)
        if pos is None:
            pos = _Position()
            p.positions[symbol] = pos

        p.cash += -price * qty if is_buy else price * qty
        signed = qty if is_buy else -qty
        net = pos.net_qty

        if net == 0 or (net > 0) == is_buy:
            # extending (or opening) — blend the average entry price
            total = abs(net) + qty
            pos.avg_price = (pos.avg_price * abs(net) + price * qty) // total
            pos.net_qty = net + signed
        else:
            # reducing / flipping — realise PnL on the closed part
            closed = min(abs(net), qty)
            per_unit = (price - pos.avg_price) if net > 0 else (pos.avg_price - price)
            p.realised_pnl += per_unit * closed
            pos.net_qty = net + signed
            if pos.net_qty == 0:
                pos.avg_price = 0
            elif (net > 0) != (pos.net_qty > 0):
                pos.avg_price = price          # flipped: rest opened at fill px

    def _order_dict_from_view(self, symbol: str, v: _COrderView) -> dict:
        return {
            "server_order_id": v.server_order_id,
            "client_order_id": v.client_order_id,
            "account_id":      v.account_id,
            "symbol":          symbol,
            "type":            _INT_TO_TYPE.get(v.type, "LIMIT"),
            "side":            _INT_TO_SIDE.get(v.side, "BID"),
            "price":           v.price,
            "trigger_price":   v.trigger_price,
            "qty_original":    v.qty_original,
            "qty_remain":      v.qty_remain,
            "qty_filled":      v.qty_filled,
            "status":          _ORDER_STATUS.get(v.status, "OPEN"),
            "created_ts_ns":   v.created_ts_ns,
            "updated_ts_ns":   v.updated_ts_ns,
        }

    def _book_levels(self, h: int, depth: int) -> tuple[list[dict], list[dict]]:
        prices = (c_int64 * depth)()
        qtys = (c_int64 * depth)()
        out: list[list[dict]] = [[], []]
        for side in (0, 1):
            n = self._dll.bridge_book_levels(h, side, depth, prices, qtys)
            out[side] = [{"price": prices[i], "qty": qtys[i]} for i in range(n)]
        return out[0], out[1]

    def _drain(self, symbol: str, h: int) -> None:
        """Pop every queued C event, translate, and fan out."""
        drained = False
        ev = _CEvent()
        while self._dll.bridge_poll_event(h, byref(ev)):
            drained = True
            if ev.kind == _EVENT_ORDER:
                # Static fields (client id, trigger, created ts) live in the
                # bridge registry; dynamic ones come from the event itself.
                view = self._dll.bridge_get_order(h, ev.order_id)
                data = {
                    "server_order_id": ev.order_id,
                    "client_order_id": view.client_order_id if view.found else 0,
                    "account_id":      ev.account_id,
                    "symbol":          symbol,
                    "type":            _INT_TO_TYPE.get(ev.type, "LIMIT"),
                    "side":            _INT_TO_SIDE.get(ev.side, "BID"),
                    "price":           ev.price,
                    "trigger_price":   view.trigger_price if view.found else 0,
                    "qty_original":    ev.qty_original,
                    "qty_remain":      ev.qty_remain,
                    "qty_filled":      ev.qty_filled,
                    "status":          _ORDER_STATUS.get(ev.status, "OPEN"),
                    "created_ts_ns":   view.created_ts_ns if view.found else ev.ts_ns,
                    "updated_ts_ns":   ev.ts_ns,
                }
                self._emit({
                    "event": "order", "channel": "orders", "symbol": symbol,
                    "user_id": data["account_id"],
                    "seq": self._seq(), "ts_ns": _now_ns(), "data": data,
                })
            elif ev.kind == _EVENT_TRADE:
                tid = (self._symbol_index.get(symbol, 0) * _TRADE_ID_SPACE
                       + ev.trade_id)
                trade = {
                    "trade_id":       tid,
                    "symbol":         symbol,
                    "price":          ev.trade_price,
                    "qty":            ev.trade_qty,
                    "aggressor_side": _INT_TO_SIDE.get(ev.aggressor_side, "BID"),
                    "aggressor_id":   ev.aggressor_id,
                    "resting_id":     ev.resting_id,
                    "ts_ns":          ev.ts_ns,
                }
                self._last_price[symbol] = ev.trade_price
                self._trades[symbol].append(trade)

                agg_buys = ev.aggressor_side == _SIDE_TO_INT["BID"]
                buyer = ev.aggressor_account_id if agg_buys else ev.resting_account_id
                seller = ev.resting_account_id if agg_buys else ev.aggressor_account_id
                self._apply_fill(buyer, symbol, True, ev.trade_price, ev.trade_qty)
                self._apply_fill(seller, symbol, False, ev.trade_price, ev.trade_qty)

                self._emit({
                    "event": "trade", "channel": "trades", "symbol": symbol,
                    "seq": self._seq(), "ts_ns": trade["ts_ns"], "data": trade,
                })

        # Anything happened => the ladder may have changed; push one book
        # delta so orderbook subscribers stay live without polling.
        if drained:
            bids, asks = self._book_levels(h, 20)
            self._emit({
                "event": "delta", "channel": "orderbook", "symbol": symbol,
                "seq": self._seq(), "ts_ns": _now_ns(),
                "data": {"bids": bids, "asks": asks},
            })

    @staticmethod
    def _ack_dict(ack: _CAck, client_order_id: int) -> dict:
        reason = ack.reject_reason.decode("utf-8", "replace") \
            if ack.reject_reason else None
        return {
            "status":          _ACK_STATUS.get(ack.status, "REJECTED"),
            "server_order_id": ack.server_order_id,
            "client_order_id": client_order_id,
            "ingress_ts_ns":   ack.ingress_ts_ns,
            "ack_ts_ns":       ack.ack_ts_ns,
            "reject_reason":   reason,
        }

    @staticmethod
    def _synthetic_reject(msg: dict, reason: str) -> dict:
        t = _now_ns()
        return {
            "status": "REJECTED", "server_order_id": 0,
            "client_order_id": msg.get("client_order_id", 0),
            "ingress_ts_ns": t, "ack_ts_ns": t, "reject_reason": reason,
        }

    # ── public API (same contract as the mock) ────────────────────────────

    async def ensure_account(self, symbol: str, account_id: int,
                             cash: int = _DEFAULT_CASH,
                             max_position: int = 0,
                             max_order_notional: int = 0) -> bool:
        """Register an account on one symbol's book with explicit funding.

        Used by the rooms layer (players get instrument-specific starting
        cash, the market-maker bot gets a very deep pocket). Idempotent —
        a second call for the same (symbol, account) is a no-op, matching
        pipeline_add_account's overwrite semantics being undesirable here.
        """
        async with self._lock:
            h = self._handle(symbol)
            if h is None:
                return False
            key = (symbol, account_id)
            if key in self._accounts_added:
                return True
            ok = bool(self._dll.bridge_add_account(
                h, account_id, cash, max_position, max_order_notional))
            if ok:
                self._accounts_added.add(key)
                # Python-side aggregation mirrors the funding so /portfolio
                # reports the right starting cash.
                p = self._portfolio_for(account_id)
                p.cash = cash
            return ok

    async def submit_order(self, msg: dict) -> dict:
        async with self._lock:
            symbol = msg["symbol"]
            h = self._handle(symbol)
            if h is None:
                return self._synthetic_reject(msg, "symbol table full or bad symbol")
            self._ensure_account(symbol, h, msg["account_id"])

            m = _CRawMsg(
                action=_ACTION_TO_INT.get(msg.get("action", "NEW"), 1),
                client_order_id=msg["client_order_id"],
                account_id=msg["account_id"],
                type=_TYPE_TO_INT[msg["type"]],
                side=_SIDE_TO_INT[msg["side"]],
                price=msg.get("price", 0),
                trigger_price=msg.get("trigger_price", 0),
                qty=msg["qty"],
                ttl_ns=msg.get("ttl_ns", 0),
            )
            ack = self._dll.bridge_submit(h, byref(m))
            if ack.status == 0 and ack.server_order_id:   # ACCEPTED
                self._order_symbol[ack.server_order_id] = symbol
            self._drain(symbol, h)
            return self._ack_dict(ack, msg["client_order_id"])

    async def cancel_order(self, order_id: int) -> tuple[str, int]:
        async with self._lock:
            symbol = self._order_symbol.get(order_id)
            if symbol is None:
                return ("not_found", 0)
            h = self._handles[symbol]
            view = self._dll.bridge_get_order(h, order_id)
            if not view.found or _ORDER_STATUS.get(view.status) in _TERMINAL:
                return ("not_found", 0)
            remain = view.qty_remain
            self._dll.bridge_cancel(h, order_id, view.account_id)
            self._drain(symbol, h)
            after = self._dll.bridge_get_order(h, order_id)
            if after.found and _ORDER_STATUS.get(after.status) == "CANCELLED":
                return ("cancelled", remain)
            return ("not_found", 0)

    async def modify_order(self, order_id: int, new_price: Optional[int],
                           new_qty: Optional[int]) -> Optional[dict]:
        async with self._lock:
            symbol = self._order_symbol.get(order_id)
            if symbol is None:
                return None
            h = self._handles[symbol]
            view = self._dll.bridge_get_order(h, order_id)
            if not view.found or _ORDER_STATUS.get(view.status) in _TERMINAL:
                return None

            m = _CRawMsg(
                action=_ACTION_TO_INT["MODIFY"],
                account_id=view.account_id,
                target_order_id=order_id,
                new_price=new_price if new_price is not None else view.price,
                new_qty=new_qty if new_qty is not None else view.qty_remain,
            )
            ack = self._dll.bridge_submit(h, byref(m))
            self._drain(symbol, h)
            if ack.status != 0:
                return None
            after = self._dll.bridge_get_order(h, order_id)
            if not after.found:
                return None
            return self._order_dict_from_view(symbol, after)

    async def get_order(self, order_id: int) -> Optional[dict]:
        async with self._lock:
            symbol = self._order_symbol.get(order_id)
            if symbol is None:
                return None
            view = self._dll.bridge_get_order(self._handles[symbol], order_id)
            if not view.found:
                return None
            return self._order_dict_from_view(symbol, view)

    async def top_of_book(self, symbol: str) -> dict:
        async with self._lock:
            h = self._handle(symbol)
            if h is None:
                return {"symbol": symbol, "bid_price": 0, "bid_qty": 0,
                        "ask_price": 0, "ask_qty": 0, "spread": -1, "mid": 0,
                        "ts_ns": _now_ns()}
            t = self._dll.bridge_top_of_book(h)
            return {
                "symbol": symbol,
                "bid_price": t.bid_price, "bid_qty": t.bid_qty,
                "ask_price": t.ask_price, "ask_qty": t.ask_qty,
                "spread": t.spread, "mid": t.mid, "ts_ns": _now_ns(),
            }

    async def book_snapshot(self, symbol: str, depth: int = 10) -> dict:
        async with self._lock:
            h = self._handle(symbol)
            bids, asks = self._book_levels(h, depth) if h is not None else ([], [])
            return {"symbol": symbol, "bids": bids, "asks": asks,
                    "seq": self._seq(), "ts_ns": _now_ns()}

    async def trades(self, symbol: str, cursor: Optional[str], limit: int
                     ) -> tuple[list[dict], Optional[str]]:
        async with self._lock:
            dq = self._trades[symbol]
            start = int(cursor) if cursor else 0
            items = list(dq)
            if start:
                items = [t for t in items if t["trade_id"] < start]
            items = sorted(items, key=lambda x: -x["trade_id"])[:limit]
            next_cur = str(items[-1]["trade_id"]) if items and len(items) == limit else None
            return (items, next_cur)

    async def portfolio(self, user_id: int) -> dict:
        async with self._lock:
            p = self._portfolio_for(user_id)
            positions = []
            for sym, pos in p.positions.items():
                last = self._last_price.get(sym, pos.avg_price)
                pos.unrealised_pnl = (last - pos.avg_price) * pos.net_qty
                positions.append({
                    "symbol": sym, "net_qty": pos.net_qty,
                    "avg_price": pos.avg_price,
                    "unrealised_pnl": pos.unrealised_pnl,
                })
            return {
                "user_id": user_id,
                "cash": p.cash,
                "realised_pnl": p.realised_pnl,
                "open_buy_notional": p.open_buy_notional,
                "open_sell_qty": p.open_sell_qty,
                "positions": positions,
            }

    async def events(self) -> AsyncIterator[dict]:
        """Async pull from the internal fan-out queue — drives the WS layer."""
        while True:
            msg = await self._event_q.get()
            yield msg


# ═══════════════════════════════════════════════════════════════════════════
# In-memory mock (fallback when the DLL isn't available)
# ═══════════════════════════════════════════════════════════════════════════

@dataclass
class _Order:
    server_order_id: int
    client_order_id: int
    account_id: int
    symbol: str
    type: str
    side: str
    price: int
    trigger_price: int
    qty_original: int
    qty_remain: int
    qty_filled: int
    status: str
    created_ts_ns: int
    updated_ts_ns: int


@dataclass
class _BookLevel:
    price: int
    qty: int


class _Engine:
    """
    Minimal deterministic mock. It is NOT a matching engine — it simulates
    only enough state (acks, book top, trades history) for the API surface
    to behave correctly for clients during integration.
    """

    def __init__(self) -> None:
        self._lock = asyncio.Lock()
        self._next_server_id = 0
        self._next_trade_id = 0
        self._next_seq = 0
        self._idem: dict[tuple[int, int], int] = {}     # (account, coid) -> server_id
        self._orders: dict[int, _Order] = {}
        self._books: dict[str, dict] = defaultdict(lambda: {"bids": [], "asks": []})
        self._trades: dict[str, deque] = defaultdict(lambda: deque(maxlen=10_000))
        self._portfolios: dict[int, _Portfolio] = {}
        self._event_q: asyncio.Queue = asyncio.Queue(maxsize=10_000)
        self._kill_switch = False

    # ── helpers ──────────────────────────────────────────────────────────
    def _seq(self) -> int:
        self._next_seq += 1
        return self._next_seq

    async def _emit(self, msg: dict) -> None:
        """Push an event onto the fan-out queue (drop-oldest on overflow)."""
        try:
            self._event_q.put_nowait(msg)
        except asyncio.QueueFull:
            # Match the C bus's philosophy: never block producers.
            try:
                _ = self._event_q.get_nowait()
                self._event_q.put_nowait(msg)
            except Exception:
                pass

    def _portfolio_for(self, user_id: int) -> _Portfolio:
        p = self._portfolios.get(user_id)
        if p is None:
            p = _Portfolio(user_id=user_id)
            self._portfolios[user_id] = p
        return p

    # ── public API ───────────────────────────────────────────────────────
    async def ensure_account(self, symbol: str, account_id: int,
                             cash: int = _DEFAULT_CASH,
                             max_position: int = 0,
                             max_order_notional: int = 0) -> bool:
        """Mock parity with _CEngine.ensure_account — sets starting cash."""
        del symbol, max_position, max_order_notional  # mock has no risk layer
        async with self._lock:
            if account_id not in self._portfolios:
                self._portfolios[account_id] = _Portfolio(
                    user_id=account_id, cash=cash)
            return True

    async def submit_order(self, msg: dict) -> dict:
        """POST /orders → returns an ack dict with status + server_order_id."""
        async with self._lock:
            t0 = _now_ns()

            if self._kill_switch and msg.get("action", "NEW") == "NEW":
                return self._ack("KILLED", 0, msg, t0, "kill-switch engaged")

            # Idempotency check
            key = (msg["account_id"], msg["client_order_id"])
            if key in self._idem:
                sid = self._idem[key]
                return self._ack("DUPLICATE", sid, msg, t0, "duplicate client_order_id")

            # Assign server_order_id + record
            self._next_server_id += 1
            sid = self._next_server_id
            self._idem[key] = sid
            o = _Order(
                server_order_id=sid,
                client_order_id=msg["client_order_id"],
                account_id=msg["account_id"],
                symbol=msg["symbol"],
                type=msg["type"],
                side=msg["side"],
                price=msg.get("price", 0),
                trigger_price=msg.get("trigger_price", 0),
                qty_original=msg["qty"],
                qty_remain=msg["qty"],
                qty_filled=0,
                status="OPEN",
                created_ts_ns=t0,
                updated_ts_ns=t0,
            )
            self._orders[sid] = o

            # Very small "matching" simulation for MARKET: consume book top.
            if o.type in ("MARKET", "LIMIT", "IOC", "FOK"):
                await self._simulate_match(o)

            # Mirror into book when a LIMIT still has remainder
            if o.type == "LIMIT" and o.qty_remain > 0:
                self._rest_order(o)

            await self._emit({
                "event": "order",
                "channel": "orders",
                "symbol": o.symbol,
                "user_id": o.account_id,
                "seq": self._seq(),
                "ts_ns": _now_ns(),
                "data": self._order_to_dict(o),
            })
            return self._ack("ACCEPTED", sid, msg, t0, None)

    async def cancel_order(self, order_id: int) -> tuple[str, int]:
        async with self._lock:
            o = self._orders.get(order_id)
            if o is None or o.status in ("CANCELLED", "FILLED", "REJECTED"):
                return ("not_found", 0)
            cancelled = o.qty_remain
            o.qty_remain = 0
            o.status = "CANCELLED"
            o.updated_ts_ns = _now_ns()
            self._remove_from_book(o)
            await self._emit({
                "event": "order",
                "channel": "orders",
                "symbol": o.symbol,
                "user_id": o.account_id,
                "seq": self._seq(),
                "ts_ns": _now_ns(),
                "data": self._order_to_dict(o),
            })
            return ("cancelled", cancelled)

    async def modify_order(self, order_id: int, new_price: Optional[int],
                           new_qty: Optional[int]) -> Optional[dict]:
        """Modify = cancel-and-re-add; mirrors the pipeline's TODO contract."""
        async with self._lock:
            o = self._orders.get(order_id)
            if o is None or o.status != "OPEN":
                return None
            self._remove_from_book(o)
            if new_price is not None:
                o.price = new_price
            if new_qty is not None:
                o.qty_original = new_qty
                o.qty_remain = max(0, new_qty - o.qty_filled)
            o.updated_ts_ns = _now_ns()
            self._rest_order(o)
            await self._emit({
                "event": "order",
                "channel": "orders",
                "symbol": o.symbol,
                "user_id": o.account_id,
                "seq": self._seq(),
                "ts_ns": _now_ns(),
                "data": self._order_to_dict(o),
            })
            return self._order_to_dict(o)

    async def get_order(self, order_id: int) -> Optional[dict]:
        async with self._lock:
            o = self._orders.get(order_id)
            return self._order_to_dict(o) if o else None

    async def top_of_book(self, symbol: str) -> dict:
        async with self._lock:
            book = self._books[symbol]
            bid = book["bids"][0] if book["bids"] else None
            ask = book["asks"][0] if book["asks"] else None
            bp, bq = (bid.price, bid.qty) if bid else (0, 0)
            ap, aq = (ask.price, ask.qty) if ask else (0, 0)
            spread = (ap - bp) if bp and ap else -1
            mid = ((ap + bp) // 2) if bp and ap else 0
            return {
                "symbol": symbol, "bid_price": bp, "bid_qty": bq,
                "ask_price": ap, "ask_qty": aq,
                "spread": spread, "mid": mid, "ts_ns": _now_ns(),
            }

    async def book_snapshot(self, symbol: str, depth: int = 10) -> dict:
        async with self._lock:
            book = self._books[symbol]
            return {
                "symbol": symbol,
                "bids": [{"price": l.price, "qty": l.qty} for l in book["bids"][:depth]],
                "asks": [{"price": l.price, "qty": l.qty} for l in book["asks"][:depth]],
                "seq": self._seq(),
                "ts_ns": _now_ns(),
            }

    async def trades(self, symbol: str, cursor: Optional[str], limit: int
                     ) -> tuple[list[dict], Optional[str]]:
        async with self._lock:
            dq = self._trades[symbol]
            start = int(cursor) if cursor else 0
            items = list(dq)
            # cursor is a trade_id exclusive upper bound for pagination
            if start:
                items = [t for t in items if t["trade_id"] < start]
            items = sorted(items, key=lambda x: -x["trade_id"])[:limit]
            next_cur = str(items[-1]["trade_id"]) if items and len(items) == limit else None
            return (items, next_cur)

    async def portfolio(self, user_id: int) -> dict:
        async with self._lock:
            p = self._portfolio_for(user_id)
            return {
                "user_id": user_id,
                "cash": p.cash,
                "realised_pnl": p.realised_pnl,
                "open_buy_notional": p.open_buy_notional,
                "open_sell_qty": p.open_sell_qty,
                "positions": [
                    {"symbol": s, "net_qty": pos.net_qty,
                     "avg_price": pos.avg_price,
                     "unrealised_pnl": pos.unrealised_pnl}
                    for s, pos in p.positions.items()
                ],
            }

    async def events(self) -> AsyncIterator[dict]:
        """Async pull from the internal fan-out queue — drives the WS layer."""
        while True:
            msg = await self._event_q.get()
            yield msg

    # ── internal: simulation helpers ─────────────────────────────────────
    async def _simulate_match(self, o: _Order) -> None:
        """Consume opposite side of the book while qty_remain > 0 and crosses."""
        book = self._books[o.symbol]
        opp = "asks" if o.side == "BID" else "bids"
        levels: list[_BookLevel] = book[opp]
        while o.qty_remain > 0 and levels:
            lv = levels[0]
            crosses = (
                (o.side == "BID" and (o.type == "MARKET" or o.price >= lv.price)) or
                (o.side == "ASK" and (o.type == "MARKET" or o.price <= lv.price))
            )
            if not crosses:
                break
            fill_qty = min(o.qty_remain, lv.qty)
            lv.qty -= fill_qty
            o.qty_remain -= fill_qty
            o.qty_filled += fill_qty
            self._next_trade_id += 1
            trade = {
                "trade_id": self._next_trade_id,
                "symbol": o.symbol,
                "price": lv.price,
                "qty": fill_qty,
                "aggressor_side": o.side,
                "aggressor_id": o.server_order_id,
                "resting_id": 0,      # mock
                "ts_ns": _now_ns(),
            }
            self._trades[o.symbol].append(trade)
            await self._emit({
                "event": "trade",
                "channel": "trades",
                "symbol": o.symbol,
                "seq": self._seq(),
                "ts_ns": trade["ts_ns"],
                "data": trade,
            })
            if lv.qty == 0:
                levels.pop(0)

        # FOK cannot partially fill
        if o.type == "FOK" and o.qty_remain > 0:
            o.status = "REJECTED"
            # roll back fills in a real engine; mock keeps it simple
            return

        if o.qty_remain == 0:
            o.status = "FILLED"
        elif o.qty_filled > 0:
            o.status = "PARTIAL" if o.type == "LIMIT" else "FILLED"
        # IOC: any remainder is implicitly cancelled by not resting

    def _rest_order(self, o: _Order) -> None:
        book = self._books[o.symbol]
        side_key = "bids" if o.side == "BID" else "asks"
        levels: list[_BookLevel] = book[side_key]
        # Keep bids desc, asks asc — stable insertion
        for i, lv in enumerate(levels):
            if lv.price == o.price:
                lv.qty += o.qty_remain
                return
            better = (o.side == "BID" and o.price > lv.price) or \
                     (o.side == "ASK" and o.price < lv.price)
            if better:
                levels.insert(i, _BookLevel(price=o.price, qty=o.qty_remain))
                return
        levels.append(_BookLevel(price=o.price, qty=o.qty_remain))

    def _remove_from_book(self, o: _Order) -> None:
        if o.qty_remain <= 0:
            return
        book = self._books[o.symbol]
        side_key = "bids" if o.side == "BID" else "asks"
        levels: list[_BookLevel] = book[side_key]
        for i, lv in enumerate(levels):
            if lv.price == o.price:
                lv.qty = max(0, lv.qty - o.qty_remain)
                if lv.qty == 0:
                    levels.pop(i)
                return

    def _ack(self, status: str, sid: int, msg: dict, t0: int,
             reason: Optional[str]) -> dict:
        return {
            "status": status,
            "server_order_id": sid,
            "client_order_id": msg.get("client_order_id", 0),
            "ingress_ts_ns": t0,
            "ack_ts_ns": _now_ns(),
            "reject_reason": reason,
        }

    def _order_to_dict(self, o: _Order) -> dict:
        return {
            "server_order_id": o.server_order_id,
            "client_order_id": o.client_order_id,
            "account_id": o.account_id,
            "symbol": o.symbol,
            "type": o.type,
            "side": o.side,
            "price": o.price,
            "trigger_price": o.trigger_price,
            "qty_original": o.qty_original,
            "qty_remain": o.qty_remain,
            "qty_filled": o.qty_filled,
            "status": o.status,
            "created_ts_ns": o.created_ts_ns,
            "updated_ts_ns": o.updated_ts_ns,
        }


# ─── Engine selection ────────────────────────────────────────────────────────

def _make_engine():
    path = settings.pipeline_dll
    if path and os.path.exists(path):
        try:
            eng = _CEngine(path)
            log.info("engine_bridge: using C pipeline DLL at %s", path)
            return eng
        except Exception as e:
            log.warning("engine_bridge: failed to load %s (%s) — "
                        "falling back to in-memory mock", path, e)
    else:
        log.info("engine_bridge: no pipeline DLL (%r) — using in-memory mock",
                 path)
    return _Engine()


# Single global bridge instance imported by routes + WS.
engine = _make_engine()
