"""
config.py — environment-backed configuration for the API layer.

All settings are read once at process start from environment variables (or
the optional .env file). Nothing here reaches out to the network — that is
the responsibility of engine_bridge and db.
"""

import os
from dataclasses import dataclass

# Load variables from a .env file if python-dotenv is installed.
# This keeps "API_TOKEN=..." out of shell history in dev.
try:
    from dotenv import load_dotenv
    load_dotenv(dotenv_path=os.path.join(os.path.dirname(__file__), ".env"))
except Exception:
    pass


@dataclass(frozen=True)
class Settings:
    # Auth
    api_token: str = os.getenv("API_TOKEN", "dev-token")

    # Postgres (optional — empty string => memory mode)
    database_url: str = os.getenv("DATABASE_URL", "")

    # HTTP
    host: str = os.getenv("HOST", "127.0.0.1")
    port: int = int(os.getenv("PORT", "8080"))

    # Dev flag — turns on reload + verbose logs when run via run.py
    dev: bool = os.getenv("DEV", "1") == "1"

    # Finnhub API key for live stock quotes (free tier: finnhub.io).
    # Empty => only crypto instruments (Binance, keyless) are offered.
    finnhub_api_key: str = os.getenv("FINNHUB_API_KEY", "")

    # C engine bridge: path to libpipeline.dll (built with `make dll`).
    # Defaults to <repo>/bridge/libpipeline.dll. Set PIPELINE_DLL= (empty)
    # to force the in-memory mock engine; a missing file also falls back
    # to the mock so the API keeps working before the first build.
    pipeline_dll: str = os.getenv(
        "PIPELINE_DLL",
        os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     "bridge", "libpipeline.dll"),
    )


# Single shared instance the rest of the app imports.
settings = Settings()
