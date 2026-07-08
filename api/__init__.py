"""Package init — installs the Windows SelectorEventLoopPolicy.

psycopg's async mode is incompatible with the default ProactorEventLoop on
Windows. We set the policy here (rather than in run.py) so it applies to
every process — including the reloader's child process that uvicorn spawns.
"""

import asyncio
import sys

if sys.platform == "win32":
    asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
