# API Layer (FastAPI)

Thin HTTP + WebSocket surface over the C trade pipeline. Uses a bearer
token for authentication and a pluggable engine bridge so we can wire
the C DLL in a later pass without touching routes.

## Background & Architecture

End-to-end trade lifecycle built around a central, non-blocking Gateway:

- **Entry Gateway**
  Normalizes inputs (REST/WS/bots) into one `gw_raw_msg_t`, does fast schema checks,
  stamps `ts_ns`, applies idempotency by `(account_id, client_order_id)`, and pushes
  to the pipeline via SPSC rings.
- **Validation + Risk (Pre-Trade)**
  Tick-size, fat-finger banding, and per-account reservations (cash/qty) to ensure
  orders are admissible before sequencing.
- **Sequencer + WAL**
  Assigns a deterministic sequence number and appends to a write-ahead log for full
  replayability and recovery.
- **Router + Matching Engine (C)**
  Price-time priority book, AVL price levels + FIFO queues, IOC/FOK/Stop(-Limit)
  handling, and last-trade price updates.
- **Trade Execution + Portfolio**
  VWAP average price, realised/unrealised P&L, and notional/qty reservations updated
  on fills and cancels.
- **Event Bus**
  Structured events fan-out to subscribers (UI/WS/analytics) with sequence numbers.

Key properties carried into this API layer:

- Non-blocking path via SPSC rings; producers never block on slow consumers.
- Determinism + Replay using a Sequencer + WAL; events include monotonic `seq`.
- Idempotency on `POST /orders` using `(account_id, client_order_id)`.
- Fixed-point integers for `price`/`qty`; callers must know the price scale.
- Timestamps as integers `*_ns` from a monotonic clock.

## Backend stack & integration plan

- Framework: FastAPI (REST + WS), optional Postgres via SQLAlchemy (async).
- Auth: Simple bearer token from `.env` (`API_TOKEN`).
- Bridge: `engine_bridge.py` isolates the API from the C layer, and picks
  its implementation once at import time based on `settings.pipeline_dll`
  (see `PIPELINE_DLL` below):
  - **Live (default)**: `ctypes` calls into `bridge/libpipeline.dll` (built
    from `bridge/bridge.c` + `pipeline/` + `engine/` — see `bridge/bridge.h`
    for the flat C API). One C pipeline instance per symbol; Python owns
    cross-symbol trade history + portfolio aggregation, fed from the
    bridge's per-symbol event queue.
  - **Fallback**: in-memory mock (`_Engine`), used automatically if the DLL
    is missing or fails to load — useful before running `make dll`.

## Data contracts (API <-> Engine)

- Orders map 1:1 to `gw_raw_msg_t` fields (snake_case).
- Acks mirror `gw_ack_t` with `status` in
  `ACCEPTED|REJECTED|DUPLICATE|THROTTLED|KILLED`.
- Order views / trades / book use integer types; no floats.
- Error envelope everywhere is `{ "error": { "code", "message", ... } }`.

## Auth model

- Private REST routes require `Authorization: Bearer <token>`.
- WebSocket private channels (`portfolio`, `orders`) require `{type:"auth", token}` first.
- Token is loaded from environment (`API_TOKEN`), read via `.env` in development.

## WebSockets design

- Single endpoint `/ws` multiplexes channels (`trades`, `orderbook`, `portfolio`, `orders`).
- Subscription model — clients opt-in per channel/symbol/user:
  `{ "type":"subscribe", "channel":"trades", "symbol":"AAPL" }`.
- Structured events — one envelope with `event`, `channel`, `symbol?`, `user_id?`,
  `seq`, `ts_ns`, `data`.
- Snapshot + delta — `orderbook` sends a `snapshot` on subscribe, then deltas.
- Sequence numbers — detect gaps; client should resubscribe on a gap.
- Backpressure — per-connection bounded queue, drop-oldest; producers never block.
- Heartbeat — server `ping` every 15s; disconnect after 45s idle unless `pong` seen.

## Order lifecycle semantics (API)

- `POST /orders` is idempotent on `(account_id, client_order_id)` and returns an ack.
- `PATCH /orders/{id}` behaves like cancel + re-add (server-managed).
- `DELETE /orders/{id}` returns `{ status: "cancelled"|"not_found", order_id, cancelled_qty }`.
- `GET /orders/{id}` provides latest server view including `qty_remain`/`qty_filled`.

## Order types supported by the engine

- LIMIT, MARKET, IOC, FOK, STOP, STOP_LIMIT

## Notes on pricing and precision

- All monetary/quantity values are integers. Price scale (e.g. 1e2 or 1e4) is a
  symbol-level convention documented outside the API; we never emit floats.

## Install (with uv)

[uv](https://docs.astral.sh/uv/) is recommended — it auto-manages the Python
version and resolves deps in seconds. From the repo root:

```powershell
cd api
uv sync                       # creates .venv + installs everything from pyproject.toml
copy .env.example .env
```

Set at least `API_TOKEN=dev-token` in `.env`.
`DATABASE_URL` is optional — leave it blank to run in memory-only mode.

Build the C engine DLL so the API drives the real matching engine instead
of the mock (from the repo root, not `api/`):

```powershell
mingw32-make dll     # or: make dll (bash/WSL)
```

This produces `bridge/libpipeline.dll` via `zig cc` (pulled from PyPI
through `uv` — no separate toolchain install needed). `PIPELINE_DLL` in
`.env` overrides the path; leave it unset to use the default
`bridge/libpipeline.dll`, or set it empty to force the mock.

> uv will fetch a compatible Python (3.11–3.13) automatically; you don't need
> to pre-install Python.

### Alternative: plain pip

```powershell
cd api
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
copy .env.example .env
```

## Run

```powershell
# uv (preferred)
uv run python -m api.run
# or directly
uv run uvicorn api.main:app --reload --port 8080
```

Health check:
```powershell
curl http://127.0.0.1:8080/health
```

## Endpoints

### Orders (auth required)
- `POST   /orders`                 — submit new order (idempotent on `client_order_id`)
- `GET    /orders/{id}`            — fetch order state
- `PATCH  /orders/{id}`            — amend price/qty
- `DELETE /orders/{id}`            — cancel

### Order book (public)
- `GET /orderbook/{symbol}`        — full ladder snapshot
- `GET /orderbook/{symbol}/top`    — best bid / ask / spread / mid
- `GET /orderbook/{symbol}/depth?levels=10`

### Trades (public)
- `GET /trades?symbol=AAPL&limit=50&cursor=…`

### Portfolio (auth required)
- `GET /portfolio/{user_id}`

### WebSocket
- `GET /ws`

## WebSocket protocol

Client -> Server:
```jsonc
{"type":"auth","token":"dev-token"}                        // required for private
{"type":"subscribe","channel":"trades","symbol":"AAPL"}
{"type":"subscribe","channel":"orderbook","symbol":"AAPL","depth":10}
{"type":"subscribe","channel":"portfolio","user_id":1}
{"type":"unsubscribe","channel":"trades","symbol":"AAPL"}
{"type":"pong"}
```

Server -> Client (single envelope for every event):
```jsonc
{
  "event": "snapshot|delta|trade|order|portfolio|ping|error|ack",
  "channel": "trades|orderbook|portfolio|orders",
  "symbol": "AAPL",
  "user_id": 1,
  "seq": 42,
  "ts_ns": 1714920000000000000,
  "data": { ... }
}
```

- `snapshot` is sent on orderbook subscribe (so the client doesn't start empty).
- `delta` + monotonic `seq` follow — resubscribe if you detect a gap.
- `ping` is sent every 15s; reply with `{"type":"pong"}` or you'll be dropped after 45s idle.

## Example

```powershell
# New limit order
$body = @{
  client_order_id = 1001
  account_id      = 1
  symbol          = "AAPL"
  type            = "LIMIT"
  side            = "ASK"
  price           = 10000
  qty             = 100
} | ConvertTo-Json

curl -X POST http://127.0.0.1:8080/orders `
     -H "Authorization: Bearer dev-token" `
     -H "Content-Type: application/json" `
     -d $body
```

## Database (Postgres)

Set `DATABASE_URL` in `api/.env`, e.g.

```
DATABASE_URL=postgresql+psycopg://postgres:postgres@localhost:5432/trading
```

### Schema

| Table              | Purpose                                                            |
|--------------------|--------------------------------------------------------------------|
| `symbols`          | Tradable instruments + risk params (price_scale, tick, fat-finger). |
| `accounts`         | Engine accounts (user_id reserved for future auth).                |
| `orders`           | Snapshot of latest state per `server_order_id`.                     |
| `order_idempotency`| Survives restarts so duplicate POSTs still return DUPLICATE.        |
| `order_events`     | Append-only lifecycle (ACCEPTED, PARTIAL, FILLED, CANCELLED, …).    |
| `trades`           | One row per match (fill).                                          |
| `portfolio_ledger` | Append-only money/qty mutations — full transaction history.         |
| `event_watermarks` | Last `seq` consumed from each event source.                         |

The pump (`main._pump_events`) writes orders, order_events, trades, ledger entries,
and updates the watermark on every engine event — best-effort, never blocks the
WebSocket fan-out.

### Migrations (Alembic)

In dev mode the API auto-creates tables (`create_all`) on startup. For production,
use Alembic:

```powershell
cd api
uv run alembic upgrade head            # apply all migrations
uv run alembic revision --autogenerate -m "add_some_column"
uv run alembic downgrade -1            # roll back one revision
```

Initial migration: `api/alembic/versions/0001_initial.py`.

## Wiring to the real C engine (done)

`engine_bridge.py` loads `bridge/libpipeline.dll` via `ctypes` and drives
one C pipeline per symbol through the flat API in `bridge/bridge.h`
(`bridge_add_symbol`, `bridge_add_account`, `bridge_submit`, `bridge_cancel`,
`bridge_get_order`, `bridge_top_of_book`, `bridge_book_levels`,
`bridge_poll_event`). All DLL calls are serialized behind one `asyncio.Lock`
because the underlying pipeline is single-threaded and not reentrant.

Division of labour: the C side is the source of truth for matching, order
state, and per-symbol books. `portfolio_t` is single-symbol by design, so
cross-symbol cash/PnL aggregation and the trade tape live in Python,
fed from the bridge's event stream (`_CEngine._drain` in `engine_bridge.py`).

Routes, WS, subscriptions, and auth are unchanged — the bridge swap was
fully contained to `engine_bridge.py` (plus a small `config.py` addition
for `PIPELINE_DLL`) as originally planned.
