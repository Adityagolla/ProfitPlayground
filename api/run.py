"""
run.py — dev entrypoint. Wraps uvicorn with our settings.

On Windows we must use the SelectorEventLoop, because psycopg's async
mode is incompatible with the default ProactorEventLoop.
"""

import asyncio
import sys
import uvicorn

from .config import settings


def main() -> None:
    # psycopg async requires SelectorEventLoop on Windows.
    if sys.platform == "win32":
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())

    uvicorn.run(
        "api.main:app",
        host=settings.host,
        port=settings.port,
        reload=settings.dev,
        log_level="info",
        loop="asyncio",   # don't let uvicorn pick proactor under us
    )


if __name__ == "__main__":
    main()
