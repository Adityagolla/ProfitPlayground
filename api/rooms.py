"""
rooms.py — face-off game rooms (FIFA-style stock duel).

Flow:
  1. Player 1: POST /rooms {duration_s} -> room code + private player token
  2. Player 2: POST /rooms/{code}/join  -> their own token
  3. Both:     POST /rooms/{code}/pick {instrument_id}   (team select)
  4. When both have picked, the match goes LIVE: each player gets their
     own order book (room-scoped engine symbol) with a market-maker bot
     quoting the live price of their chosen instrument. Identical
     starting cash, shared countdown.
  5. Full time: trading locks, positions are marked at the last live
     price, and the higher net P&L wins.

Rooms are in-process state (like the engine itself). Restarting the API
ends all matches — acceptable for a 2-player game server.
"""

from __future__ import annotations
import asyncio
import secrets
import string
import time
from dataclasses import dataclass, field
from typing import Optional

from fastapi import APIRouter, Depends, HTTPException
from fastapi.security import HTTPBearer, HTTPAuthorizationCredentials
from pydantic import BaseModel, Field

from . import persistence
from .engine_bridge import engine
from .instruments import available, by_id, cached_quotes
from .market_maker import BookState, run_market_maker

router = APIRouter(tags=["rooms"])

# 5-char codes; alphabet avoids 0/O, 1/I/L so codes survive being read aloud.
_CODE_ALPHABET = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"
CODE_LEN = 5

PLAYER_CASH = 10_000_000          # $100k at scale 100 — identical for both
ALLOWED_DURATIONS_S = (300, 600, 900)
DEFAULT_DURATION_S = 600
MAX_ENGINE_ACCOUNTS = 64          # keep in sync with PIPELINE_MAX_ACCOUNTS


@dataclass
class Player:
    player_id: int                # 1 or 2
    token: str
    account_id: int
    bot_account_id: int
    instrument_id: Optional[str] = None
    engine_symbol: Optional[str] = None
    book: BookState = field(default_factory=BookState)
    bot_task: Optional[asyncio.Task] = None


@dataclass
class Room:
    code: str
    duration_s: int
    created_at: float = field(default_factory=time.time)
    status: str = "waiting"       # waiting -> picking -> live -> finished
    ends_at: Optional[float] = None
    players: dict[int, Player] = field(default_factory=dict)
    final: Optional[dict] = None  # frozen scoreboard once finished


class RoomRegistry:
    def __init__(self) -> None:
        self.rooms: dict[str, Room] = {}
        self.tokens: dict[str, tuple[str, int]] = {}   # token -> (code, pid)
        self._next_account = 1
        self._lock = asyncio.Lock()

    # ── lifecycle ────────────────────────────────────────────────────────
    async def create(self, duration_s: int) -> tuple[Room, Player]:
        async with self._lock:
            if self._next_account + 4 > MAX_ENGINE_ACCOUNTS:
                raise HTTPException(503, detail={"error": {
                    "code": "SERVER_FULL",
                    "message": "no room capacity left — restart the server"}})
            code = self._new_code()
            room = Room(code=code, duration_s=duration_s)
            p1 = self._new_player(room, 1)
            self.rooms[code] = room
            return room, p1

    async def join(self, code: str) -> tuple[Room, Player]:
        async with self._lock:
            room = self._room_or_404(code)
            if 2 in room.players:
                raise HTTPException(409, detail={"error": {
                    "code": "ROOM_FULL", "message": "both seats are taken"}})
            if room.status == "finished":
                raise HTTPException(409, detail={"error": {
                    "code": "MATCH_OVER", "message": "match already finished"}})
            p2 = self._new_player(room, 2)
            room.status = "picking"
            return room, p2

    def _new_player(self, room: Room, pid: int) -> Player:
        p = Player(
            player_id=pid,
            token=secrets.token_urlsafe(24),
            account_id=self._next_account,
            bot_account_id=self._next_account + 1,
        )
        self._next_account += 2
        room.players[pid] = p
        self.tokens[p.token] = (room.code, pid)
        return p

    def _new_code(self) -> str:
        while True:
            code = "".join(secrets.choice(_CODE_ALPHABET)
                           for _ in range(CODE_LEN))
            if code not in self.rooms:
                return code

    # ── lookups ──────────────────────────────────────────────────────────
    def _room_or_404(self, code: str) -> Room:
        room = self.rooms.get(code.upper())
        if room is None:
            raise HTTPException(404, detail={"error": {
                "code": "ROOM_NOT_FOUND", "message": f"no room {code!r}"}})
        return room

    def resolve_token(self, token: str) -> Optional[tuple[Room, Player]]:
        hit = self.tokens.get(token)
        if not hit:
            return None
        room = self.rooms.get(hit[0])
        if room is None:
            return None
        return room, room.players[hit[1]]

    # ── match control ────────────────────────────────────────────────────
    async def pick(self, room: Room, player: Player, instrument_id: str) -> None:
        if room.status in ("live", "finished"):
            raise HTTPException(409, detail={"error": {
                "code": "ALREADY_LIVE", "message": "match already started"}})
        instr = by_id(instrument_id)
        if instr is None:
            raise HTTPException(404, detail={"error": {
                "code": "UNKNOWN_INSTRUMENT",
                "message": f"{instrument_id!r} is not on the roster"}})
        player.instrument_id = instr.id
        # Room-scoped, player-scoped book: "<code><pid>:<instr>" <= 15 chars.
        player.engine_symbol = f"{room.code}{player.player_id}:{instr.id}"

        if (2 in room.players
                and all(p.instrument_id for p in room.players.values())):
            await self._go_live(room)

    async def _go_live(self, room: Room) -> None:
        room.ends_at = time.time() + room.duration_s
        for p in room.players.values():
            instr = by_id(p.instrument_id)          # validated in pick()
            assert p.engine_symbol and instr
            await engine.ensure_account(p.engine_symbol, p.account_id,
                                        cash=PLAYER_CASH)
            # DB reference rows so persistence FKs resolve for this book.
            await persistence.ensure_seed(
                [p.engine_symbol], [p.account_id, p.bot_account_id])
            p.bot_task = asyncio.create_task(
                run_market_maker(p.engine_symbol, instr, p.bot_account_id,
                                 p.book, room.ends_at),
                name=f"mm-{p.engine_symbol}")
        room.status = "live"

    async def finish_if_due(self, room: Room) -> None:
        if room.status == "live" and time.time() >= (room.ends_at or 0):
            await self.finish(room)

    async def finish(self, room: Room) -> None:
        if room.status == "finished":
            return
        room.status = "finished"
        for p in room.players.values():
            if p.bot_task:
                p.bot_task.cancel()
        room.final = await self._scoreboard(room)

    async def shutdown(self) -> None:
        for room in self.rooms.values():
            for p in room.players.values():
                if p.bot_task:
                    p.bot_task.cancel()

    # ── scoring ──────────────────────────────────────────────────────────
    async def _player_score(self, p: Player) -> dict:
        pf = await engine.portfolio(p.account_id)
        position_qty = 0
        mark = p.book.mark_price
        pos_value = 0
        for pos in pf["positions"]:
            if pos["symbol"] != p.engine_symbol:
                continue
            position_qty = pos["net_qty"]
            px = mark if mark is not None else pos["avg_price"]
            pos_value += pos["net_qty"] * px
        net = pf["cash"] + pos_value - PLAYER_CASH
        return {
            "player_id": p.player_id,
            "instrument_id": p.instrument_id,
            "cash": pf["cash"],
            "position_qty": position_qty,
            "mark_price": mark,
            "live_price": p.book.live_price,
            "net_pnl": net,
        }

    async def _scoreboard(self, room: Room) -> dict:
        players = {}
        for pid in sorted(room.players):
            players[str(pid)] = await self._player_score(room.players[pid])
        winner = None
        if len(players) == 2:
            a, b = players["1"]["net_pnl"], players["2"]["net_pnl"]
            winner = 1 if a > b else 2 if b > a else 0   # 0 = draw
        return {"players": players, "winner": winner}


registry = RoomRegistry()

# ─── Auth: player bearer tokens ──────────────────────────────────────────────

_bearer = HTTPBearer(auto_error=False)


def require_player(
    creds: HTTPAuthorizationCredentials | None = Depends(_bearer),
) -> tuple[Room, Player]:
    resolved = (registry.resolve_token(creds.credentials)
                if creds and creds.scheme.lower() == "bearer" else None)
    if resolved is None:
        raise HTTPException(401, detail={"error": {
            "code": "UNAUTHORIZED", "message": "invalid player token"}})
    return resolved


# ─── Schemas ─────────────────────────────────────────────────────────────────

class CreateRoomRequest(BaseModel):
    duration_s: int = Field(DEFAULT_DURATION_S)


class PickRequest(BaseModel):
    instrument_id: str = Field(..., min_length=1, max_length=7)


# ─── Routes ──────────────────────────────────────────────────────────────────

@router.get("/instruments")
async def list_instruments() -> dict:
    """The roster: every pickable instrument with a live quote."""
    instrs = available()
    quotes = await cached_quotes(instrs)
    return {"items": [{
        "id": i.id,
        "display": i.display,
        "name": i.name,
        "feed": i.feed,
        "lot_label": i.lot_label,
        "quote": quotes.get(i.id),
    } for i in instrs]}


def _room_public(room: Room) -> dict:
    return {
        "code": room.code,
        "status": room.status,
        "duration_s": room.duration_s,
        "ends_at": room.ends_at,
        "remaining_s": (max(0, int(room.ends_at - time.time()))
                        if room.ends_at else None),
        "players": {str(pid): {
            "joined": True,
            "picked": p.instrument_id is not None,
            "instrument_id": p.instrument_id,
        } for pid, p in room.players.items()},
    }


def _session(room: Room, p: Player) -> dict:
    return {
        "code": room.code,
        "player_id": p.player_id,
        "token": p.token,
        "account_id": p.account_id,
        "instrument_id": p.instrument_id,
        "symbol": p.engine_symbol,
        "room": _room_public(room),
    }


@router.post("/rooms", status_code=201)
async def create_room(body: CreateRoomRequest) -> dict:
    if body.duration_s not in ALLOWED_DURATIONS_S:
        raise HTTPException(422, detail={"error": {
            "code": "BAD_DURATION",
            "message": f"duration_s must be one of {ALLOWED_DURATIONS_S}"}})
    room, p1 = await registry.create(body.duration_s)
    return _session(room, p1)


@router.post("/rooms/{code}/join")
async def join_room(code: str) -> dict:
    room, p2 = await registry.join(code)
    return _session(room, p2)


@router.post("/rooms/{code}/pick")
async def pick_instrument(code: str, body: PickRequest,
                          auth: tuple[Room, Player] = Depends(require_player),
                          ) -> dict:
    room, player = auth
    if room.code != code.upper():
        raise HTTPException(403, detail={"error": {
            "code": "WRONG_ROOM", "message": "token is for another room"}})
    await registry.pick(room, player, body.instrument_id)
    return _session(room, player)


@router.get("/rooms/{code}")
async def room_state(code: str) -> dict:
    room = registry._room_or_404(code)
    await registry.finish_if_due(room)
    return _room_public(room)


@router.get("/rooms/{code}/me")
async def my_session(code: str,
                     auth: tuple[Room, Player] = Depends(require_player),
                     ) -> dict:
    room, player = auth
    if room.code != code.upper():
        raise HTTPException(403, detail={"error": {
            "code": "WRONG_ROOM", "message": "token is for another room"}})
    await registry.finish_if_due(room)
    return _session(room, player)


@router.get("/rooms/{code}/score")
async def room_score(code: str) -> dict:
    room = registry._room_or_404(code)
    await registry.finish_if_due(room)
    if room.status == "finished" and room.final is not None:
        board = room.final
    elif room.status == "live":
        board = await registry._scoreboard(room)
    else:
        board = {"players": {}, "winner": None}
    return {**_room_public(room), **board}
