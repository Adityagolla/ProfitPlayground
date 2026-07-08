"""
routes_ws.py — Single WebSocket endpoint at /ws.

Protocol (client -> server):
  {"type":"auth","token":"<bearer>"}                     (required for private)
  {"type":"subscribe","channel":"trades","symbol":"AAPL"}
  {"type":"subscribe","channel":"orderbook","symbol":"AAPL","depth":10}
  {"type":"subscribe","channel":"portfolio","user_id":1}
  {"type":"unsubscribe","channel":"trades","symbol":"AAPL"}
  {"type":"pong"}                                        (reply to server ping)

Protocol (server -> client): one envelope for every message —
  {
    "event":"snapshot|delta|trade|order|portfolio|ping|error|ack",
    "channel":"trades|orderbook|portfolio|orders",
    "symbol"?:"AAPL",
    "user_id"?:1,
    "seq": 123,
    "ts_ns": 1714920000000000000,
    "data": {...}
  }

Backpressure: slow clients get drops recorded in `client.dropped`, never
block the matching engine.
"""

from __future__ import annotations
import asyncio
import json
import time

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from .auth import resolve_token
from .engine_bridge import engine
from .subscriptions import manager, Subscription

router = APIRouter()

HEARTBEAT_EVERY_S = 15
MAX_IDLE_S = 45


def _now_ns() -> int:
    return time.monotonic_ns()


async def _sender(client, ws: WebSocket) -> None:
    """Drain this client's outbound queue -> wire. One task per client."""
    while True:
        msg = await client.out_q.get()
        await ws.send_text(json.dumps(msg, separators=(",", ":")))


async def _heartbeat(client, ws: WebSocket) -> None:
    """Periodic ping; disconnect if the client goes silent too long."""
    while True:
        await asyncio.sleep(HEARTBEAT_EVERY_S)
        try:
            await ws.send_text(json.dumps({"event": "ping", "ts_ns": _now_ns()}))
        except Exception:
            return


async def _send_orderbook_snapshot(client, symbol: str, depth: int) -> None:
    """Initial book dump so the client never starts from an empty slate."""
    snap = await engine.book_snapshot(symbol, depth)
    msg = {
        "event": "snapshot",
        "channel": "orderbook",
        "symbol": symbol,
        "seq": snap["seq"],
        "ts_ns": snap["ts_ns"],
        "data": {"bids": snap["bids"], "asks": snap["asks"]},
    }
    try:
        client.out_q.put_nowait(msg)
    except asyncio.QueueFull:
        client.dropped += 1


async def _handle_client_msg(client, raw: str, ws: WebSocket) -> None:
    """Parse and act on one client -> server control message."""
    try:
        m = json.loads(raw)
    except Exception:
        await ws.send_text(json.dumps({
            "event": "error",
            "data": {"code": "BAD_JSON", "message": "invalid JSON"},
        }))
        return

    mtype = m.get("type")

    if mtype == "auth":
        principal = resolve_token(m.get("token", ""))
        if principal is not None:
            client.authed = True
            # Players get a server-derived identity (their engine account);
            # only the admin token may impersonate an arbitrary user_id.
            client.user_id = (principal.account_id
                              if principal.kind == "player"
                              else m.get("user_id"))
            await ws.send_text(json.dumps({
                "event": "ack",
                "data": {"type": "auth", "ok": True,
                         "user_id": client.user_id},
            }))
        else:
            await ws.send_text(json.dumps({
                "event": "error",
                "data": {"code": "UNAUTHORIZED", "message": "bad token"},
            }))
        return

    if mtype == "subscribe":
        ch = m.get("channel")
        sym = m.get("symbol")
        uid = m.get("user_id")
        if ch in ("portfolio", "orders") and not client.authed:
            await ws.send_text(json.dumps({
                "event": "error",
                "data": {"code": "UNAUTHORIZED", "message": "auth required for private channel"},
            }))
            return
        await manager.subscribe(client, Subscription(channel=ch, symbol=sym, user_id=uid))
        await ws.send_text(json.dumps({
            "event": "ack",
            "data": {"type": "subscribe", "channel": ch, "symbol": sym, "user_id": uid},
        }))
        # Orderbook subscription: bootstrap with a snapshot.
        if ch == "orderbook" and sym:
            await _send_orderbook_snapshot(client, sym, int(m.get("depth", 20)))
        return

    if mtype == "unsubscribe":
        await manager.unsubscribe(client, m.get("channel"),
                                  m.get("symbol"), m.get("user_id"))
        await ws.send_text(json.dumps({
            "event": "ack", "data": {"type": "unsubscribe"},
        }))
        return

    if mtype == "pong":
        return  # used to cancel idle timers — not tracked in the scaffold

    await ws.send_text(json.dumps({
        "event": "error",
        "data": {"code": "UNKNOWN_TYPE", "message": f"{mtype!r} not recognised"},
    }))


@router.websocket("/ws")
async def ws_endpoint(ws: WebSocket) -> None:
    """The only WebSocket entry point. Multiplexes all channels."""
    await ws.accept()
    client = await manager.add(ws)

    sender_task = asyncio.create_task(_sender(client, ws))
    hb_task     = asyncio.create_task(_heartbeat(client, ws))

    try:
        while True:
            raw = await asyncio.wait_for(ws.receive_text(), timeout=MAX_IDLE_S)
            await _handle_client_msg(client, raw, ws)
    except (WebSocketDisconnect, asyncio.TimeoutError):
        pass
    finally:
        sender_task.cancel()
        hb_task.cancel()
        await manager.remove(client)
