# RunMe — How to Compile and Run Everything

## What's in This Project (current layout)

```
ProfitPlayground antigrav/
│
├── engine/                  # Core order book + matching engine
│   ├── orderbook.h, orderbook.c
│   ├── matching_engine.h, matching_engine.c
│   └── orders.h, orders.c
│
├── pipeline/                # Gateway + Risk + Portfolio + Bus + Orchestrator
│   ├── gateway.h, gateway.c
│   ├── risk.h, risk.c
│   ├── portfolio.h, portfolio.c
│   ├── event_bus.h, event_bus.c
│   └── pipeline.h, pipeline.c
│
├── demo/
│   └── pipeline_demo.c      # End-to-end demo (Pipeline → Engine → Bus)
│
├── bridge/                  # Flat ctypes-friendly C API over the pipeline
│   ├── bridge.h, bridge.c   # Built into bridge/libpipeline.dll (`make dll`)
│
├── api/                     # FastAPI + Postgres surface (uv-managed)
│   ├── main.py, run.py, config.py
│   ├── models.py, db.py, persistence.py
│   ├── routes_*.py, schemas.py, subscriptions.py
│   ├── alembic/             # migrations
│   ├── pyproject.toml       # deps (uv sync --project api)
│   └── .env.example
│
├── docs/
│   └── explaination.md      # Architecture and rationale
│
├── matching_engine_test.c   # 15 tests for the matching engine
├── order_main.c             # 5 smoke tests for the raw order book only
├── Makefile                 # MinGW/GCC: build and run demo
├── changes.md               # Full list of every change made and why
└── RunMe.md                 # This file
```

---

## Requirements

- **GCC** (MinGW on Windows, or GCC on Linux/macOS)
- No external libraries — everything is standard C99

Check GCC is installed:
```bash
gcc --version
```
You should see something like `gcc (MinGW.org...) 9.2.0` or similar.

---

## Run 0 — Pipeline Demo (Gateway → Validation → Risk → Sequencer/WAL → Router → Matching → Portfolio → Event Bus)

This runs the full real-time pipeline and prints trades, book updates, gateway acks, and final portfolio stats.

### Option A: Using Makefile (recommended)

```powershell
make
make run
```

### Option B: Direct gcc (no make)

```powershell
gcc -O2 -Wall -Wno-unused-function -Iengine -Ipipeline -o demo/pipeline_demo.exe \
    demo/pipeline_demo.c \
    pipeline/gateway.c pipeline/risk.c pipeline/portfolio.c pipeline/event_bus.c pipeline/pipeline.c \
    engine/matching_engine.c engine/orders.c engine/orderbook.c

./demo/pipeline_demo.exe
```

### Expected Output (excerpt)

```
=== 1. Maker (acct=1) places LIMIT ASK 100 @ 10000 ===
  [GW] ACCEPTED  server_id=1  reason=-
  [UI] ACCEPT  id=1 side=1 qty=100->100 filled=0
  [UI] BOOK   bid=0(0)  ask=10000(100)

=== 2. Taker (acct=2) MARKET BUY 30 ===
  [GW] ACCEPTED  server_id=2  reason=-
  [UI] TRADE  trade_id=1  price=10000  qty=30  acct=2
  [UI] FILLED  id=2 side=0 qty=30->0 filled=30
...
  Account 1  cash=...  net_qty=...  avg_px=...  realised=...  unreal@last=...
  Account 2  cash=...  net_qty=...  avg_px=...  realised=...  unreal@last=...
```

---

## Run 1 — Matching Engine Full Test Suite (15 tests)

> **This is the main test. Run this first.**

### Step 1: Compile

```bash
gcc -Wall -Wextra -O2 -Iengine -o me_test.exe \
  matching_engine_test.c engine/matching_engine.c engine/orderbook.c engine/orders.c
```

- `-Wall -Wextra` — enable all warnings (should be **zero warnings** on a clean build)
- `-O2` — optimise (same as production)
- `-o me_test.exe` — output binary name

### Step 2: Run

```bash
./me_test.exe
```

### Expected Output

```
╔════════════════════════════════════════════╗
║     Matching Engine — Full Test Suite      ║
╚════════════════════════════════════════════╝

══ Test 1: Limit order — no cross, order rests
  Submitted BID LIMIT 200 @ 150.00
  [ACCEPTED] #1 BID LIMIT  price=150.0000  orig=200  filled=0  remain=200
  [TOB] bid=150.0000 x 200  ask=0.0000 x 0  spread=-0.0001
  ✓ PASS

══ Test 2: Limit order — full cross
  Resting ASK: sell 100 @ 200.00
  Aggressive BID: buy 100 @ 201.00
  [TRADE] trade#1  agg=#2  rest=#1  price=200.0000  qty=100
  [FILLED] #2 BID LIMIT  price=201.0000  orig=100  filled=100  remain=0
  ✓ PASS

══ Test 3: Limit order — partial cross, remainder rests
  [TRADE] trade#1  agg=#2  rest=#1  price=300.0000  qty=50
  [PARTIAL] #2 BID LIMIT  price=300.0000  orig=200  filled=50  remain=150
  ✓ PASS

══ Test 4: Market order — sweeps multiple ask levels
  [TRADE] trade#1  agg=#4  rest=#1  price=400.0000  qty=30
  [TRADE] trade#2  agg=#4  rest=#2  price=400.1000  qty=50
  [TRADE] trade#3  agg=#4  rest=#3  price=400.2000  qty=40
  [FILLED] #4 BID MARKET  price=0.0000  orig=120  filled=120  remain=0
  ✓ PASS

══ Test 5: IOC — partial fill, remainder cancelled (not resting)
  [TRADE] trade#1  agg=#2  rest=#1  price=180.0000  qty=40
  [CANCELLED] #2 BID IOC  ...
  ✓ PASS

══ Test 6: FOK — fill-or-kill logic
  [FILLED] #2 BID FOK  ...  filled=50  remain=0
  [REJECTED] #0 BID FOK  ...  reason="FOK: insufficient liquidity"
  ✓ PASS

══ Test 7: Price-Time priority
  [TRADE] trade#1  agg=#5  rest=#1  qty=10   ← A filled first (oldest)
  [TRADE] trade#2  agg=#5  rest=#2  qty=20   ← B filled second
  [TRADE] trade#3  agg=#5  rest=#3  qty=5    ← C partially filled last
  ✓ PASS

══ Test 8: Cancel and Modify via engine API
  Modified #1 → 500 @ 451.00
  Cancelled #1
  ✓ PASS

══ Test 9: Validation — bad params
  qty=0: [REJECTED] reason="qty must be > 0"
  Limit price=0: [REJECTED] reason="limit price must be > 0"
  Market with price!=0: [REJECTED] reason="market order must have price = 0"
  ✓ PASS

══ Test 10: Full trading scenario
  (builds a BTC/USD book, sweeps 4000 qty across 3 bid levels)
  ✓ PASS

══ Test 11: Stop-market triggers on price touch
  ...
  ✓ PASS

══ Test 12: Stop-limit triggers → enters as limit
  ...
  ✓ PASS

══ Test 13: Stop cancel before trigger
  ...
  ✓ PASS

══ Test 14: Day order expiry
  ...
  ✓ PASS

══ Test 15: Day order fills before expiry
  ...
  ✓ PASS

╔════════════════════════════════════════════╗
║          All 15 tests passed  ✓           ║
╚════════════════════════════════════════════╝
```

If you see **all 15 ✓ PASS** and no `error:` or `warning:` in the compiler output, everything is working.

---

## Run 2 — Raw Order Book Smoke Tests (5 tests)

This tests the order book layer directly, without the matching engine on top.

### Step 1: Compile

```bash
gcc -Wall -Wextra -O2 -Iengine -o ob_demo.exe order_main.c engine/orderbook.c
```

### Step 2: Run

```bash
./ob_demo.exe
```

### Expected Output

```
+==========================================+
|  Order Book Phase 2 — Smoke Test Suite   |
|  Pools + Cache-friendly + Threading-ready|
+==========================================+

  sizeof(ob_book_t)  = 255080 bytes
  sizeof(ob_order_t) = 56 bytes
  sizeof(ob_level_t) = 32 bytes

=== TEST 1: resting limit orders (no match) ===
  Bid #1 resting @ 149.50 x 100
  Bid #2 resting @ 149.00 x 200
  Bid #3 resting @ 148.75 x 150
  Ask #4 resting @ 150.00 x 80
  Ask #5 resting @ 150.50 x 300
  best bid=149.5000  best ask=150.0000  spread=0.5000
  ...
  PASS

=== TEST 2: cancel order ===
  ...
  PASS

=== TEST 3: modify order ===
  ...
  PASS

=== TEST 4: FIFO priority within same price level ===
  ...
  PASS

=== TEST 5: pool utilisation ===
  Pool orders free: 3996 / 4096
  Pool orders free: 4096 / 4096   ← all slots recovered after cancel
  PASS

  All tests passed.
```

Key things to check in Test 5:
- After adding 100 orders: `Pool orders free: 3996 / 4096` (100 slots used)
- After cancelling all: `Pool orders free: 4096 / 4096` (all slots recovered — no leaks)

---

## What Each Test Checks

### Matching Engine Tests (`matching_engine_test.c`)

| Test | What it checks |
|------|----------------|
| **Test 1** | A limit order with no opposite side rests correctly; ACCEPTED event emitted |
| **Test 2** | A crossing limit order fully fills the resting side; TRADE + FILLED events |
| **Test 3** | Partial cross: aggressor fills what's available, remainder rests; PARTIAL event |
| **Test 4** | Market order sweeps 3 ask levels in one shot; 3 TRADE events, all at different prices |
| **Test 5** | IOC: fills 40 of 100 requested, remainder cancelled (never rests in book) |
| **Test 6** | FOK fills when liquidity exists; rejects when it doesn't — nothing touches the book on rejection |
| **Test 7** | Price-Time priority: 3 orders at same price (A, B, C); aggressor fills A first, then B, then C |
| **Test 8** | Cancel and Modify go through the engine API; correct events emitted; book state updated |
| **Test 9** | All 3 validation rules (zero qty, zero limit price, market with price) reject cleanly |
| **Test 10** | Full BTC/USD scenario: build 6-level book, sweep 4000 qty across 3 bid levels |
| **Test 11** | Stop-market order activates and enters as a market order when last trade price hits trigger |
| **Test 12** | Stop-limit order activates and enters as a resting limit order when last trade price hits trigger |
| **Test 13** | Stop order can be cancelled while in dormant state (before it triggers) |
| **Test 14** | Day order with `ttl_ns` is auto-cancelled when `me_expire_orders` is called after its expiry |
| **Test 15** | Day order fills normally if hit by aggressive liquidity before its expiry time |

### Book Smoke Tests (`order_main.c`)

| Test | What it checks |
|------|----------------|
| **Test 1** | Bids and asks rest at correct levels; best bid/ask/spread correct |
| **Test 2** | Cancel removes order; double-cancel returns NOT_FOUND |
| **Test 3** | Modify price and qty; snapshot reflects new values |
| **Test 4** | FIFO: first order submitted at a price level has earlier timestamp |
| **Test 5** | Pool slots are recovered after cancel-all (memory leak check) |

---

## Optional: Enable Threading

Compile with `-DOB_ENABLE_THREADS` to activate the per-book read-write spinlock:

```bash
gcc -Wall -Wextra -O2 -DOB_ENABLE_THREADS -o me_test.exe matching_engine_test.c matching_engine.c orderbook.c orders.c
./me_test.exe
```

All 15 tests still pass. The spinlock is transparent — it just adds thread safety at zero cost when single-threaded because no contention occurs.

---

## How to Tell If Something Is Broken

| Symptom | Likely cause |
|---------|-------------|
| `error: 'order_book.h': No such file` | Wrong include — should be `orderbook.h` |
| `warning: unknown conversion type character 'l'` | Missing `__USE_MINGW_ANSI_STDIO 1` at top of file |
| `Assertion failed` in a test | A quantity, price, or event count didn't match expected value |
| Pool levels free doesn't return to max after cancel-all | Pool slot leak in `ob_cancel_order` |
| All 10 tests show PASS but some fills are 0 | FOK pre-flight or matching loop is not crossing correctly |
| Stack overflow on startup | `me_engine_t` or `ob_book_t` declared as a local variable — must be heap-allocated |

---

## Run 3 — Build `bridge/libpipeline.dll` (wires the API to the real engine)

The FastAPI layer (`api/engine_bridge.py`) drives the real matching engine
through this DLL via `ctypes`. Without it, the API silently falls back to
an in-memory mock — so build this before Run 4 if you want real fills.

```powershell
make dll
```

or without `make`:

```bash
mingw32-make dll
```

Default build uses `zig cc` (pulled from PyPI through `uv` — no separate
64-bit toolchain to install). This matters on Windows: the DLL is loaded
into a 64-bit Python venv via `ctypes`, so it must be a genuine 64-bit
build — the 32-bit MinGW.org `gcc` used for the demo/test `.exe`s above
produces a DLL Python can't load (`WinError 193`). If you have your own
64-bit gcc/clang, override it:

```powershell
make dll CC64="x86_64-w64-mingw32-gcc"
```

> Note: an Anaconda `m2w64-toolchain` gcc (5.3) segfaults (internal
> compiler error) compiling this codebase's `double`-returning functions —
> if `make dll` ICEs, that's likely why; the zig cc default avoids it.

Verify it built and exports the bridge API:

```powershell
python -c "import ctypes; d = ctypes.CDLL('bridge/libpipeline.dll'); d.bridge_add_symbol; print('ok')"
```

---

## Run 4 — FastAPI Layer + Postgres (uv-managed)

The Python API in `api/` exposes the C pipeline over REST + WebSockets and
persists every order / event / trade / ledger row into Postgres.

### One-time setup

1. Install [uv](https://docs.astral.sh/uv/) (handles Python + venv automatically).
2. Create the database in Postgres (run as the `postgres` user):

   ```sql
   CREATE DATABASE profitplayground
       WITH OWNER = postgres
            ENCODING = 'UTF8'
            CONNECTION LIMIT = -1
            IS_TEMPLATE = False;
   ```

3. Configure `api/.env` (copy from `api/.env.example` if missing):

   ```
   API_TOKEN=dev-token
   DATABASE_URL=postgresql+psycopg://postgres:1234@localhost:5432/profitplayground
   HOST=127.0.0.1
   PORT=8080
   DEV=1
   # Optional: path to the DLL from Run 3. Unset => defaults to
   # <repo>/bridge/libpipeline.dll. Set to empty to force the mock engine.
   #PIPELINE_DLL=
   ```

4. Sync deps (creates `api/.venv` and pulls Python 3.13 if missing):

   ```powershell
   uv sync --project api
   ```

### Start the API server

```powershell
uv run --project api python -m api.run
```

You should see:
```
INFO:     Uvicorn running on http://127.0.0.1:8080 (Press CTRL+C to quit)
INFO:     Application startup complete.
```

In dev mode the schema is `create_all`-ed automatically. Reference rows
(`symbols`, `accounts`) are seeded on startup.

### Useful endpoints

```powershell
# Liveness
curl http://127.0.0.1:8080/health

# Swagger UI
start http://127.0.0.1:8080/docs

# Public market data
curl http://127.0.0.1:8080/orderbook/AAPL
curl http://127.0.0.1:8080/orderbook/AAPL/top
curl "http://127.0.0.1:8080/trades?symbol=AAPL&limit=10"

# Submit an order (auth required)
$body = @{
  client_order_id = 1001
  account_id      = 1
  symbol          = "AAPL"
  type            = "LIMIT"
  side            = "ASK"
  price           = 10000
  qty             = 50
} | ConvertTo-Json

curl -Method POST http://127.0.0.1:8080/orders `
  -Headers @{ "Authorization"="Bearer dev-token"; "Content-Type"="application/json" } `
  -Body $body

# Fetch / cancel
curl http://127.0.0.1:8080/orders/1     -H "Authorization: Bearer dev-token"
curl -Method DELETE http://127.0.0.1:8080/orders/1 -H "Authorization: Bearer dev-token"

# Portfolio
curl http://127.0.0.1:8080/portfolio/1  -H "Authorization: Bearer dev-token"
```

### Test the WebSocket (browser console)

```js
const ws = new WebSocket("ws://127.0.0.1:8080/ws");
ws.onmessage = e => console.log("WS:", e.data);
ws.onopen = () => {
  ws.send(JSON.stringify({type:"auth", token:"dev-token"}));
  ws.send(JSON.stringify({type:"subscribe", channel:"orderbook", symbol:"AAPL", depth:10}));
  ws.send(JSON.stringify({type:"subscribe", channel:"trades",    symbol:"AAPL"}));
  ws.send(JSON.stringify({type:"subscribe", channel:"portfolio", user_id:1}));
};
```

You should see a `snapshot` event immediately, then `delta` / `trade` /
`order` / `portfolio` events as you POST orders. A `ping` arrives every 15s.

### Migrations (Alembic)

Dev mode auto-creates tables. For controlled changes:

```powershell
uv run --project api alembic upgrade head            # apply all migrations
uv run --project api alembic revision --autogenerate -m "add_some_column"
uv run --project api alembic downgrade -1            # roll back one revision
```

Initial migration: `api/alembic/versions/0001_initial.py`.

### Add / remove Python deps

```powershell
uv add    --project api <package>
uv remove --project api <package>
uv sync   --project api          # re-resolve after editing pyproject.toml manually
```

### Inspect the database

```powershell
# Open psql against the project DB
psql -U postgres -d profitplayground

# Inside psql:
\dt                         -- list tables (8 expected)
select count(*) from orders;
select * from orders order by server_order_id desc limit 5;
select * from order_events order by event_id desc limit 10;
select * from trades order by trade_id desc limit 10;
select * from portfolio_ledger where account_id=1 order by ts_ns desc;
```

### Smoke-test the whole pipe

With the server running, post a sell + a crossing buy and watch rows
appear:

```powershell
$body = @{ client_order_id=2001; account_id=1; symbol="AAPL"; type="LIMIT"; side="ASK"; price=10000; qty=50 } | ConvertTo-Json
curl -Method POST http://127.0.0.1:8080/orders -Headers @{ "Authorization"="Bearer dev-token"; "Content-Type"="application/json" } -Body $body

$body = @{ client_order_id=2002; account_id=2; symbol="AAPL"; type="LIMIT"; side="BID"; price=10000; qty=30 } | ConvertTo-Json
curl -Method POST http://127.0.0.1:8080/orders -Headers @{ "Authorization"="Bearer dev-token"; "Content-Type"="application/json" } -Body $body

# Now check the DB:
psql -U postgres -d profitplayground -c "select count(*) from orders, order_events, trades, portfolio_ledger;"
```

Expected: rows in `orders`, `order_events`, `trades`, and 2 ledger rows
per fill.

### Troubleshooting (Windows-specific)

| Symptom | Fix |
|---------|-----|
| `Psycopg cannot use the 'ProactorEventLoop'` | Already handled in `api/__init__.py` — make sure you're starting via `python -m api.run` (not raw `uvicorn`). |
| `ModuleNotFoundError: No module named 'uvicorn'` | You ran `uv run` from the wrong directory. Use `--project api`. |
| Pip install fails with "Microsoft C++ Build Tools" | You're on Python 3.14 with no wheels. Use `uv sync --project api` — it pins to 3.13. |
| `relation "orders" does not exist` | Server didn't reach DB. Check `DATABASE_URL` in `api/.env` and that Postgres is running on 5432. |
| `DUPLICATE` ack on a fresh run | The `(account_id, client_order_id)` pair already exists in `order_idempotency`. Use new `client_order_id` values or `truncate order_idempotency;`. |

---

## Run 5 — Frontend (2-player trading dashboard)

Dark-themed dashboard: order book ladder, canvas price chart, order
ticket, trade tape, positions. Requires Run 3 (`make dll`) + Run 4
(the API server on port 8080) to be running first.

```powershell
cd frontend
npm install
npm run dev
```

Open http://localhost:5173. Vite's dev server proxies `/api/*` and `/ws`
to `127.0.0.1:8080`, so no CORS setup or separate `.env` is needed to get
started. Each player opens their own browser tab and picks **Player 1**
or **Player 2** from the top-bar account switcher (persisted per-browser
via `localStorage`) — both share the same live order book over one
WebSocket feed. See `frontend/README.md` for the full layout breakdown.

---

## Run 5 — Two-Player Face-Off (frontend + rooms + live prices)

FIFA-style stock duel: each player picks an instrument, gets an identical
$100k, and trades its **live price** (via a per-book market-maker bot) for
5/10/15 minutes. Higher net P&L wins.

### Start everything

```powershell
# 1. API (rooms + engine + bots) — needs the DLL from Run 3
uv run --project api python -m api.run

# 2. Frontend dev server
cd frontend
npm install        # first time only
npm run dev        # http://localhost:5173
```

### Play

1. Player 1 opens `http://localhost:5173`, picks a match length, clicks
   **Create match**, and shares the 5-char room code (click it to copy).
2. Player 2 opens the same URL (use `npm run dev -- --host` + your LAN IP
   for a second machine) and joins with the code.
3. Both pick an instrument from the roster (live prices shown). The match
   starts the moment both have picked.
4. Trade. The scoreboard at the top shows both net P&Ls and the countdown.
   At full time trading locks and the winner screen appears.

### Live price feeds

- **Crypto (BTC/ETH/SOL)** — Binance public API, no key needed, moves 24/7.
- **Stocks (NVDA, PLTR, TSLA, META, AVGO, JPM, GS)** — set
  `FINNHUB_API_KEY` in `api/.env` (free key from finnhub.io). Stocks only
  move during US market hours; they're hidden from the roster without a key.

Each player's book gets a market-maker bot that re-quotes bid/ask around
the live price every couple of seconds — you're trading against the real
market, and against your rival's judgement.

---

## Quick Reference — One-Liners

```bash
# Build & run the full pipeline demo
make && make run

# Direct gcc (no make): build & run pipeline demo
gcc -O2 -Wall -Wno-unused-function -Iengine -Ipipeline -o demo/pipeline_demo.exe \
    demo/pipeline_demo.c pipeline/gateway.c pipeline/risk.c pipeline/portfolio.c \
    pipeline/event_bus.c pipeline/pipeline.c engine/matching_engine.c engine/orders.c engine/orderbook.c && \
    ./demo/pipeline_demo.exe

# Build and run matching engine tests
gcc -Wall -Wextra -O2 -Iengine -o me_test.exe matching_engine_test.c \
    engine/matching_engine.c engine/orderbook.c engine/orders.c && ./me_test.exe

# Build and run book smoke tests
gcc -Wall -Wextra -O2 -Iengine -o ob_demo.exe order_main.c engine/orderbook.c && ./ob_demo.exe

# Clean built binaries (PowerShell + bash compatible)
del demo\pipeline_demo.exe 2> NUL; rm -f demo/pipeline_demo.exe 2>/dev/null
del me_test.exe ob_demo.exe 2> NUL; rm -f me_test.exe ob_demo.exe 2>/dev/null

# ── API + Postgres ────────────────────────────────────────────

# First-time setup
uv sync --project api

# Run the API server (REST + WebSocket)
uv run --project api python -m api.run

# Migrations
uv run --project api alembic upgrade head
uv run --project api alembic revision --autogenerate -m "msg"
uv run --project api alembic downgrade -1

# Manage Python deps
uv add    --project api <package>
uv remove --project api <package>

# ── Frontend ──────────────────────────────────────────────────

# First-time setup + run
cd frontend && npm install && npm run dev

# Quick DB peek
psql -U postgres -d profitplayground -c "\dt"
psql -U postgres -d profitplayground -c "select * from orders order by server_order_id desc limit 5;"
```

---

*See `changes.md` for a full explanation of every design decision and code change.*
