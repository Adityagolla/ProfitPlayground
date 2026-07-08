"""
db.py — optional async SQLAlchemy engine.

If DATABASE_URL is empty, the API runs in memory-only mode. Writes
silently no-op so the app can boot and serve requests with no Postgres.
"""

from __future__ import annotations
import logging
from contextlib import asynccontextmanager

from .config import settings

log = logging.getLogger("api.db")

_engine = None
_session_maker = None


def is_enabled() -> bool:
    return _session_maker is not None


def _redact(url: str) -> str:
    if "@" in url and "//" in url:
        head, tail = url.split("//", 1)
        creds, host = tail.split("@", 1)
        return f"{head}//***@{host}"
    return url


async def init() -> None:
    """Create the async engine. In DEV mode also create_all tables."""
    global _engine, _session_maker
    if not settings.database_url:
        log.info("db: DATABASE_URL not set — running memory-only")
        return
    try:
        from sqlalchemy.ext.asyncio import (
            create_async_engine, async_sessionmaker,
        )
        from . import models  # noqa: F401 (registers tables on Base)

        _engine = create_async_engine(
            settings.database_url, echo=False, future=True, pool_pre_ping=True,
        )
        _session_maker = async_sessionmaker(_engine, expire_on_commit=False)

        if settings.dev:
            async with _engine.begin() as conn:
                await conn.run_sync(models.Base.metadata.create_all)
            log.info("db: schema ensured (dev create_all)")

        log.info("db: connected to %s", _redact(settings.database_url))
    except Exception as e:  # pragma: no cover
        log.warning("db: init failed (%s) — running memory-only", e)
        _engine = None
        _session_maker = None


async def shutdown() -> None:
    global _engine, _session_maker
    if _engine is not None:
        await _engine.dispose()
    _engine = None
    _session_maker = None


@asynccontextmanager
async def session():
    """Async context manager. Yields a session or None when DB is off."""
    if _session_maker is None:
        yield None
        return
    async with _session_maker() as s:
        try:
            yield s
        except Exception:
            await s.rollback()
            raise
