"""
auth.py — Bearer-token authentication.

Two kinds of principals:
  - admin  — the single API_TOKEN from env/.env. Full access (dev tooling,
             bots run in-process and don't use HTTP at all).
  - player — a per-player room token minted by rooms.py. Locked to that
             player's engine account and their room's book; the server
             overrides account/symbol on every order so a player can't
             trade as their opponent.

Matches the C gateway's philosophy: cheap, deterministic, no I/O on the
hot path.
"""

from __future__ import annotations
from dataclasses import dataclass
from typing import Any, Optional

from fastapi import Depends, HTTPException, status
from fastapi.security import HTTPBearer, HTTPAuthorizationCredentials

from .config import settings

# FastAPI's built-in helper. We intentionally do NOT auto-error so we can
# return our own error envelope, consistent with the rest of the API.
_bearer = HTTPBearer(auto_error=False)


@dataclass(frozen=True)
class Principal:
    kind: str                       # "admin" | "player"
    room: Any = None                # rooms.Room   (players only)
    player: Any = None              # rooms.Player (players only)

    @property
    def account_id(self) -> Optional[int]:
        return self.player.account_id if self.player else None


def resolve_token(token: str) -> Optional[Principal]:
    """Token string -> Principal, or None if it matches nothing."""
    if token and token == settings.api_token:
        return Principal(kind="admin")
    # Imported lazily: rooms.py has no dependency on auth, so this cannot
    # recurse — it just avoids a hard import cycle at module load.
    from .rooms import registry
    hit = registry.resolve_token(token)
    if hit is not None:
        room, player = hit
        return Principal(kind="player", room=room, player=player)
    return None


def _unauthorized() -> HTTPException:
    return HTTPException(
        status_code=status.HTTP_401_UNAUTHORIZED,
        detail={"error": {"code": "UNAUTHORIZED",
                          "message": "invalid or missing bearer token"}},
        headers={"WWW-Authenticate": "Bearer"},
    )


def require_principal(
    creds: HTTPAuthorizationCredentials | None = Depends(_bearer),
) -> Principal:
    """Accept either the admin API token or a room player token."""
    if creds is None or creds.scheme.lower() != "bearer":
        raise _unauthorized()
    principal = resolve_token(creds.credentials)
    if principal is None:
        raise _unauthorized()
    return principal


def require_bearer(
    creds: HTTPAuthorizationCredentials | None = Depends(_bearer),
) -> str:
    """Admin-only variant (kept for endpoints that predate rooms)."""
    if (
        creds is None
        or creds.scheme.lower() != "bearer"
        or creds.credentials != settings.api_token
    ):
        raise _unauthorized()
    return creds.credentials


def check_token_value(token: str) -> bool:
    """Plain string compare used by the WS layer (no HTTP objects there)."""
    return resolve_token(token) is not None
