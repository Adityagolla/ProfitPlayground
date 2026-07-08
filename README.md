# ProfitPlayground

A FIFA-style 1v1 trading duel. Two players each pick a stock or crypto
instrument, get identical starting cash, and trade its **real live
price** for a timed match. Whoever grows their P&L more before the
clock hits zero wins.

Under the hood it's a real order-matching engine — not a simulation.
Every fill comes from an actual price-time-priority C matching engine,
driven through a FastAPI + WebSocket layer, with a market-maker bot
quoting each player's book around the live market price (Binance for
crypto, Finnhub for stocks).

**Want to just play?** → [PLAY.md](PLAY.md) — full setup + how-to-play
guide, written for a friend with no prior context.

---

## Architecture

```
Browser (React)  ──HTTP/WS──▶  FastAPI (api/)  ──ctypes──▶  libpipeline.dll
                                     │                            │
                                Postgres                    C pipeline
                              (optional;                   (bridge/ ─┐
                            memory-only if                          │
                             unconfigured)                    pipeline/  ─┐
                                                                          │
                                                                    engine/
```

| Layer | Where | What it does |
|---|---|---|
| **Matching engine** | `engine/` | Price-time priority order book: AVL price levels, FIFO queues, pooled allocation (zero `malloc` on the hot path). LIMIT / MARKET / IOC / FOK / STOP / STOP_LIMIT, TTL expiry. |
| **Pipeline** | `pipeline/` | Orchestrates one order end-to-end: gateway → validation → risk → sequencer/WAL → matching → portfolio → event bus. |
| **Bridge** | `bridge/` | Flat, ctypes-friendly C API (`bridge.h`) compiled to `libpipeline.dll`. One pipeline instance per symbol; this is the only thing Python talks to. |
| **API** | `api/` | FastAPI: REST + WebSocket surface, game rooms (join codes, per-player tokens), market-maker bots, optional Postgres persistence. |
| **Frontend** | `frontend/` | Vite + React + TypeScript: scroll-animated landing page, create/join lobby, live trading dashboard with countdown scoreboard. |

See [production.md](production.md) for the monitoring/logging/testing
readiness checklist if you're taking this beyond casual play with a
friend.

---

## Quick start

### Prerequisites

- **[uv](https://docs.astral.sh/uv/)** — manages the Python backend and its virtualenv.
- **[Node.js](https://nodejs.org/)** (v18+) — runs the frontend.
- A C toolchain for the demo/test binaries (MinGW/GCC on Windows, GCC on Linux/macOS). Building the DLL that the API actually uses does **not** need this — see below.

### 1. Build the matching engine DLL

The API talks to the real engine through `bridge/libpipeline.dll`. Build it once:

```powershell
make dll
```

This defaults to `zig cc` (pulled from PyPI via `uv` — no separate 64-bit
toolchain install needed). Override with `make dll CC64="x86_64-w64-mingw32-gcc"`
if you have your own.

### 2. Backend

```powershell
uv sync --project api
copy api\.env.example api\.env
uv run --project api python -m api.run
```

`DATABASE_URL` in `api/.env` can be left blank — the game runs entirely
in memory without Postgres. Set it (and have Postgres running) if you
want every order/trade permanently persisted.

### 3. Frontend

```powershell
cd frontend
npm install
npm run dev
```

Open `http://localhost:5173`. Two players = two browser tabs, or see
[PLAY.md](PLAY.md) for playing across devices on the same Wi-Fi.

---

## Project structure

```
ProfitPlayground/
├── engine/          Core order book + matching engine (C99)
├── pipeline/         Gateway → Risk → Portfolio → Event Bus → orchestrator
├── bridge/          ctypes-friendly C API → bridge/libpipeline.dll
├── demo/            Standalone end-to-end pipeline demo
├── api/             FastAPI + Postgres surface (uv-managed)
│   ├── rooms.py         Game rooms, join codes, player tokens
│   ├── market_maker.py  Live-price quoting bot
│   ├── instruments.py   Tradable roster + Binance/Finnhub feeds
│   └── engine_bridge.py ctypes bridge (falls back to an in-memory mock)
├── frontend/        Vite + React + TypeScript UI
├── matching_engine_test.c   15 engine tests
├── order_main.c             5 order-book smoke tests
├── Makefile
├── PLAY.md          How to set up and play with a friend
├── production.md    Monitoring/logging/testing readiness checklist
└── explaination.md  Deeper architecture rationale
```

---

## Testing

```powershell
# Matching engine (15 tests)
gcc -Wall -Wextra -O2 -Iengine -o me_test.exe matching_engine_test.c engine/matching_engine.c engine/orderbook.c engine/orders.c
./me_test.exe

# Order book smoke tests (5 tests)
gcc -Wall -Wextra -O2 -Iengine -o ob_demo.exe order_main.c engine/orderbook.c
./ob_demo.exe

# Pipeline demo (gateway → engine → bus, end to end)
make run
```

All 20 C tests should pass with zero compiler warnings on a clean build.

---

## API surface

Full endpoint reference and WebSocket protocol: [api/README.md](api/README.md).

Quick smoke test once the backend is running:

```powershell
curl http://127.0.0.1:8080/health
curl http://127.0.0.1:8080/instruments
```
