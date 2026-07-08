"""
subscriptions.py — WebSocket subscription manager.

Mirrors the C event_bus: clients register a (channel, symbol|user_id)
tuple, we push matching events to them. The difference is this runs in
Python asyncio and fans out per-client via independent queues.

Backpressure policy: if a client's queue is full, we DROP the message
for that client and increment a counter. Matching engine throughput is
never coupled to slow clients.
"""

from __future__ import annotations
import asyncio
from dataclasses import dataclass, field
from typing import Optional

from .observability import WS_CLIENTS, WS_DROPPED


@dataclass
class Subscription:
    channel: str                      # "trades" | "orderbook" | "portfolio" | "orders"
    symbol: Optional[str] = None
    user_id: Optional[int] = None


@dataclass(eq=False)                  # identity hash — Clients live in a set
class Client:
    ws: object                        # starlette WebSocket
    authed: bool = False
    user_id: Optional[int] = None
    subs: list[Subscription] = field(default_factory=list)
    out_q: asyncio.Queue = field(default_factory=lambda: asyncio.Queue(maxsize=1024))
    dropped: int = 0


class SubscriptionManager:
    """Tracks every connected WS client and their subscriptions."""

    def __init__(self) -> None:
        self._clients: set[Client] = set()
        self._lock = asyncio.Lock()

    async def add(self, ws) -> Client:
        client = Client(ws=ws)
        async with self._lock:
            self._clients.add(client)
            WS_CLIENTS.set(len(self._clients))
        return client

    async def remove(self, client: Client) -> None:
        async with self._lock:
            self._clients.discard(client)
            WS_CLIENTS.set(len(self._clients))

    async def subscribe(self, client: Client, sub: Subscription) -> None:
        # No duplicates.
        if sub in client.subs:
            return
        client.subs.append(sub)

    async def unsubscribe(self, client: Client, channel: str,
                          symbol: Optional[str] = None,
                          user_id: Optional[int] = None) -> None:
        client.subs = [
            s for s in client.subs
            if not (s.channel == channel
                    and s.symbol == symbol
                    and s.user_id == user_id)
        ]

    async def broadcast(self, msg: dict) -> None:
        """Fan an event out to every client whose subscriptions match."""
        async with self._lock:
            targets = list(self._clients)

        for c in targets:
            if not self._matches(c, msg):
                continue
            try:
                c.out_q.put_nowait(msg)
            except asyncio.QueueFull:
                c.dropped += 1
                WS_DROPPED.inc()

    @staticmethod
    def _matches(c: Client, msg: dict) -> bool:
        """Return True if any of `c.subs` matches this event."""
        ch = msg.get("channel")
        sym = msg.get("symbol")
        uid = msg.get("user_id")
        for s in c.subs:
            if s.channel != ch:
                continue
            if s.symbol is not None and s.symbol != sym:
                continue
            if s.user_id is not None and s.user_id != uid:
                continue
            # Private channel requires auth
            if ch in ("portfolio", "orders") and not c.authed:
                continue
            # Player identity is server-derived (routes_ws auth): a client
            # bound to an account only ever receives that account's
            # private events, regardless of what it subscribed to.
            if (ch in ("portfolio", "orders")
                    and c.user_id is not None
                    and uid != c.user_id):
                continue
            return True
        return False


# Module-level singleton used by routes and the /ws handler.
manager = SubscriptionManager()
