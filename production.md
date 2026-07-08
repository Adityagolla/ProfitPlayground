# Production Readiness Playbook

Living document. Every time we ship something — a new metric, a load-test run, a
postmortem — the relevant section here gets updated. **Treat this file as a
checklist, not a tutorial.**

Status legend: ✅ done · 🟡 partial / in-progress · ❌ not started

---

## 0. Scope

Three production targets in this repo:

| Target            | Where                  | Critical traits                                    |
|-------------------|------------------------|----------------------------------------------------|
| C trade pipeline  | `engine/`, `pipeline/` | Sub-µs hot path, deterministic, never blocks      |
| FastAPI surface   | `api/`                 | Low p99 latency, idempotent, never drops events    |
| Postgres store    | `profitplayground` DB  | Durable, append-heavy, never the bottleneck        |

The three production pillars below apply to all of them: **monitoring**,
**logging**, **testing**.

### 0.1 What each component actually does

**C trade pipeline (`engine/`, `pipeline/`)** — the matching engine. This is
the part that takes an order, looks at the order book, and decides whether it
crosses with a resting order to produce a trade. Written in C99 with no
dependencies. It runs in-process (today as part of the demo binary, later as a
DLL the API loads). Every microsecond here multiplies: a 10 µs slowdown per
match × 100k matches/sec = 1 full CPU core wasted. So we measure it in
nanoseconds and treat it like a real-time system.

**FastAPI surface (`api/`)** — the public-facing layer. It speaks HTTP/REST
(`POST /orders`, `GET /orderbook/AAPL`) and WebSockets (`/ws` for live
trades/orderbook/portfolio updates). It is the **only** thing clients ever
touch; they never call C code directly. This layer also does the boring-but-
critical stuff: authentication, schema validation, idempotency, persistence to
Postgres, and fanning out engine events to subscribed WebSocket clients.

**Postgres store (`profitplayground` DB)** — durable history. The C engine is
fast but volatile (RAM only). Postgres holds the audit trail: every order, every
state change (`order_events`), every fill (`trades`), and every cash/qty
movement (`portfolio_ledger`). If the API process crashes, this is what we
replay from to rebuild reality. Players also read their history from here.

### 0.2 Glossary — what those "critical traits" actually mean

| Term | What it means | Why it matters here |
|------|---------------|---------------------|
| **Hot path** | The few lines of code that run on every single order. For us: gateway → risk → matching → event emit. | One slow function on the hot path makes *every* order slow. Profile it; don't `printf` from it. |
| **Sub-µs** | Less than 1 microsecond (1 µs = 1000 ns = 0.001 ms). | At 100k orders/sec you have 10 µs per order *total*. Matching has to be a small fraction of that. |
| **Deterministic** | Same input always produces the same output, in the same order. | Lets us **replay** events to rebuild state, write reproducible tests, and prove a fill was correct after the fact. |
| **Never blocks** | The hot path never waits on disk, network, or a lock that another thread holds. | A 5 ms disk write would stall 500+ orders. We push slow work (logging, DB writes) onto a different thread. |
| **p50 / p95 / p99 / p99.9 latency** | Percentile latencies. p99 = 99% of requests are faster than this number. | Averages lie — they hide tail latencies. **One** slow request out of 100 can ruin a player's experience. p99 is the honest number to optimise. |
| **Low p99 latency** | Specifically, p99 of `POST /orders` < a few ms. | Players notice anything > 50 ms. Bots notice > 1 ms. |
| **Idempotent** | Submitting the *same* order twice produces *one* result, not two. | Networks retry. If a client hits "buy" once but the request retries silently, we must not buy twice. Enforced via `order_idempotency` table on `(account_id, client_order_id)`. |
| **Never drops events** | Every engine event (order accepted/filled/cancelled, every trade) reaches both Postgres and every subscribed WebSocket. | A dropped fill = wrong portfolio = real money problem. |
| **Durable** | Once Postgres acknowledges a write, it survives crashes / power loss. | Combined with the C engine being in-RAM, durability is *only* in Postgres. Lose it and you lose history. |
| **Append-heavy** | Almost all writes are `INSERT`s, rarely `UPDATE`s, never `DELETE`s. | Append-only tables are easy to scale (partition by time, archive old chunks) and easy to audit. Our `order_events`, `trades`, `portfolio_ledger` are all append-only. |
| **Never the bottleneck** | Postgres latency must stay smaller than engine latency. | If DB writes back up, the event pump lags, WebSocket clients see stale data, and eventually we run out of memory. Measured via the **pump lag** metric (§2.1). |
| **Saturation** | A resource (CPU, DB connections, memory, queue space) is near 100% used. | Last warning before things start failing. Always alert on saturation *before* errors. |
| **Cardinality** (logging/metrics) | How many distinct label values a metric can take. | A metric labelled by `account_id` with 1M users = 1M time-series in Prometheus = OOM. Keep label sets tiny and bounded. |
| **WAL** | Write-Ahead Log. A serialized stream of every change, written before it's applied. | Postgres uses WAL internally; we use the same idea conceptually: `order_events` is our pipeline's WAL — replay it and you get the current state. |
| **Back-pressure** | Slowing the producer when the consumer can't keep up. | If a slow WebSocket client can't drain messages, we must drop it (or buffer with a cap), not let it stall the whole fan-out. |
| **Event pump** | The async task in `api/main.py` that drains engine events and (a) persists them and (b) broadcasts to WebSocket clients. | Single point where everything funnels through — first place to watch when things go wrong. |
| **SLO / budget** | Service Level Objective. A target number you commit to (e.g. "p99 ack < 5 ms"). | Without a number, "fast" is a feeling, not a fact. SLOs make regressions visible. |

---

## 1. Production-readiness checklist

| Area | Item | Status |
|------|------|--------|
| **Monitoring** | App metrics endpoint (`/metrics` Prometheus) on FastAPI | ❌ |
| | C engine latency histogram (matching loop p50/p99/p99.9) | ❌ |
| | Postgres `pg_stat_statements` enabled + dashboards | ❌ |
| | Liveness probe `/health` | ✅ (`@c:\0.Coding_pt2\ProfitPlayground antigrav\api\main.py:106-108`) |
| | Readiness probe `/ready` (DB ping) | ❌ |
| | WebSocket connection-count + queue-depth gauges | ❌ |
| | Alert rules (error rate, p99 latency, lag, DB conn) | ❌ |
| **Logging** | Structured JSON logging for FastAPI | ❌ |
| | Per-request correlation id (`X-Request-Id`) | ❌ |
| | Audit log: every order/event/trade in Postgres | ✅ (`@c:\0.Coding_pt2\ProfitPlayground antigrav\api\persistence.py`) |
| | C engine event log (stderr or shared-mem ring) with timestamps | 🟡 (printf in demo) |
| | Log rotation policy in production | ❌ |
| **Testing** | C engine unit tests | ✅ (`matching_engine_test.c` — 15 tests) |
| | Order-book smoke tests | ✅ (`order_main.c` — 5 tests) |
| | API unit tests (pytest + httpx) | ❌ |
| | API integration test against ephemeral Postgres | ❌ |
| | HTTP load test (k6) | ❌ |
| | WebSocket load test | ❌ |
| | C engine micro-benchmark (rdtsc / `clock_gettime`) | ❌ |
| | DB write-throughput benchmark | ❌ |
| **Ops** | Dockerfile for the API | ❌ |
| | Environment-driven config (no hardcoded secrets) | ✅ (`@c:\0.Coding_pt2\ProfitPlayground antigrav\api\config.py`) |
| | DB migrations gated behind Alembic | ✅ (`@c:\0.Coding_pt2\ProfitPlayground antigrav\api\alembic`) |
| | CI: build + tests on push | ❌ |

Tick items off as you ship them. **Don't lower the bar — add new rows.**

---

## 2. Monitoring

### 2.1 What to measure (the golden signals + our KPIs)

Standard four golden signals — measure on every component:

- **Latency** — request → response time. Track p50 / p95 / p99 / p99.9.
- **Traffic** — req/s, orders/s, fills/s, WS msgs/s.
- **Errors** — 4xx/5xx rate, rejected order rate, DB write failures.
- **Saturation** — queue depth, event-bus backlog, DB connection pool usage.

App-specific KPIs (track these too):

- **Order ack latency** — `ingress_ts_ns → ack_ts_ns` p99.
- **Match latency** — submit → first trade event p99.
- **Event-pump lag** — engine `seq` vs DB watermark `last_seq`.
- **WebSocket fan-out lag** — event timestamp vs broadcast timestamp.
- **Idempotency hit rate** — DUPLICATE acks / total POSTs.
- **Reservation rejections** — risk rejects / total POSTs.
- **DB write latency** — order_events insert p99.

### 2.2 Stack (recommended)

Start small; add pieces as you outgrow them.

```
FastAPI ─┬─ /metrics (prometheus_client) ─┐
         └─ structured JSON logs ──────────┤
C engine ── periodic CSV / UDP packets ────┼─► Prometheus ─► Grafana
Postgres ── postgres_exporter ─────────────┘                  └─► Alertmanager
```

If a full Prometheus stack is overkill in dev: log a one-line JSON metric every
N events (`{"metric":"match_p99","value_us":7.4}`) and parse it later. Easy to
upgrade.

### 2.3 Instrumenting FastAPI

Add `prometheus-fastapi-instrumentator` (≈30 lines):

```python
# api/main.py (sketch — not yet implemented)
from prometheus_fastapi_instrumentator import Instrumentator
Instrumentator().instrument(app).expose(app, "/metrics")
```

Default metrics: `http_requests_total`, `http_request_duration_seconds`. Add a
custom histogram for the order pipeline:

```python
from prometheus_client import Histogram, Counter
ORDER_ACK_LATENCY = Histogram("order_ack_latency_seconds",
                              "ingress→ack", buckets=(.0001,.0005,.001,.005,.01,.05,.1))
ORDER_REJECTED    = Counter  ("order_rejected_total", "rejections", ["reason"])
```

Record from `routes_orders.py` after the bridge ack. Cardinality discipline:
**don't** label by `client_order_id` or `account_id` — labels must be bounded.

### 2.4 Instrumenting the C engine

The matching loop is the hottest path; never `printf` from inside it. Pattern:

1. Inside the engine, accumulate latency samples in a thread-local **ring**:
   ```c
   static uint64_t lat_samples[1024];
   static uint32_t lat_idx;
   uint64_t t0 = rdtsc();
   me_match(...);
   lat_samples[lat_idx++ & 1023] = rdtsc() - t0;
   ```
2. A **slow path** (every 1s, on a different thread) summarises the ring into
   p50/p99/p99.9 and emits one CSV/UDP/event line.
3. The Python event pump reads those summaries and re-publishes as Prometheus
   gauges.

Avoid: lock contention, syscalls, `malloc` on hot path.

### 2.5 Postgres monitoring

```sql
-- one-time
CREATE EXTENSION IF NOT EXISTS pg_stat_statements;

-- top 10 slowest queries
SELECT round(total_exec_time::numeric, 1) AS total_ms,
       calls,
       round(mean_exec_time::numeric, 2)  AS mean_ms,
       query
FROM pg_stat_statements
ORDER BY total_exec_time DESC
LIMIT 10;
```

Watch:
- `pg_stat_activity` for long-running queries.
- `pg_stat_user_tables.n_dead_tup` for vacuum lag (orders is heavy on UPDATE).
- `pg_locks` for contention if WAL replay starts blocking writes.

In production add `postgres_exporter` and dashboard the standard panels.

### 2.6 Alerting (initial rules)

Start with five alerts. Don't go past ten until you've tuned these.

| Alert | Condition | Why |
|-------|-----------|-----|
| API 5xx rate | `rate(http_requests_total{status=~"5.."}[5m]) > 0.01` | regression / DB outage |
| Order ack p99 high | `histogram_quantile(0.99, order_ack_latency_seconds_bucket) > 0.05` | hot-path slow |
| Pump lag growing | `engine_seq - db_watermark > 1000` | persistence falling behind |
| DB pool saturated | `pg_pool_in_use / pg_pool_size > 0.9` | will block soon |
| WS dropped clients | `rate(ws_drops_total[5m]) > 1` | back-pressure failure |

---

## 3. Logging

### 3.1 Levels and policy

| Level   | Use for                                                         |
|---------|-----------------------------------------------------------------|
| ERROR   | Anything that requires human action; always paired with context |
| WARNING | Recoverable but suspicious (DUPLICATE order, retried DB write)  |
| INFO    | One-line per request / per accepted order                       |
| DEBUG   | Off in prod; turn on per-module via env                         |

Rule: every log line has enough context to grep — `account_id`, `server_order_id`, `seq` if applicable.

### 3.2 Structured logging for FastAPI

Recommend `structlog` + `python-json-logger`. Example shape:

```json
{"ts":"2026-05-06T20:00:00.123Z","level":"INFO","logger":"api.orders",
 "event":"order_accepted","server_order_id":42,"account_id":1,
 "symbol":"AAPL","side":"ASK","price":10000,"qty":50,
 "request_id":"a1b2c3","latency_ms":0.7}
```

### 3.3 Correlation IDs

Add middleware that:
1. Reads `X-Request-Id` header, or generates a UUID.
2. Stores it in a `contextvars.ContextVar`.
3. Echoes it back in the response.
4. structlog attaches it to every log line emitted while the request is in scope.

This is the single highest-leverage observability change for a small team.

### 3.4 Audit log (already done)

Every order, event, trade, and ledger entry is persisted in Postgres
(`@c:\0.Coding_pt2\ProfitPlayground antigrav\api\persistence.py`). That **is**
the audit log — no separate file. To export for regulators or replay:

```sql
copy (select * from order_events where ts_ns >= $1 order by event_id)
to '/tmp/order_events_2026-05-06.csv' with csv header;
```

### 3.5 C engine logging

Hot path: never log. Slow path (init, shutdown, periodic stats): write
timestamped lines to `stderr`. In production redirect to a file with `logrotate`
(Linux) or scheduled rotation (Windows). Keep last N=14 days.

### 3.6 Log aggregation

Two acceptable options:
- **Cheap**: ship JSON logs to stdout, let Docker / systemd capture, scrape
  with `vector` or `promtail` → Loki.
- **Cloud**: ship to managed (CloudWatch / Datadog / Grafana Cloud).

Pick the one that's already paid for in your environment.

---

## 4. Testing

### 4.1 Pyramid

```
        e2e (1) ──── full pipeline through real Postgres
       integ (2) ─── API + DB + mock engine
      unit (10) ─── per-function, fast, no I/O
load (separate) ── perf, run on demand or nightly
```

### 4.2 C engine

Already excellent (see `@c:\0.Coding_pt2\ProfitPlayground antigrav\matching_engine_test.c` — 15 tests).
Next steps:
- Add **property-based** fuzz tests (random order streams, assert invariants:
  bid_top ≤ ask_top, qty conservation, monotonic seq).
- Add **deterministic replay** test: feed a recorded WAL and assert identical
  events come out.

### 4.3 FastAPI unit tests (pytest + httpx)

```python
# api/tests/test_orders.py (sketch — not yet implemented)
import pytest
from httpx import AsyncClient, ASGITransport
from api.main import app

@pytest.mark.asyncio
async def test_post_order_accepted():
    async with AsyncClient(transport=ASGITransport(app), base_url="http://t") as c:
        r = await c.post("/orders",
            headers={"Authorization": "Bearer dev-token"},
            json={"client_order_id":1, "account_id":1, "symbol":"AAPL",
                  "type":"LIMIT", "side":"ASK", "price":10000, "qty":50})
        assert r.status_code == 202
        assert r.json()["status"] == "ACCEPTED"
```

Run with:

```powershell
uv add --project api --dev pytest pytest-asyncio httpx
uv run --project api pytest api/tests -q
```

### 4.4 Integration tests against real Postgres

Use `testcontainers-python` so each CI run gets a clean DB:

```python
from testcontainers.postgres import PostgresContainer

@pytest.fixture(scope="session")
def pg():
    with PostgresContainer("postgres:16") as pg:
        yield pg.get_connection_url(driver="psycopg")
```

Override `DATABASE_URL` per-test, run `db.init()` (which `create_all`s), then
exercise the full POST → DB row chain.

### 4.5 Load testing the API (k6)

k6 handles HTTP **and** WebSockets in one tool — perfect for our two surfaces.

```js
// load/orders.js
import http from "k6/http";
import { check, sleep } from "k6";

export const options = {
  stages: [
    { duration: "30s", target: 50  },
    { duration: "2m",  target: 500 },
    { duration: "30s", target: 0   },
  ],
  thresholds: {
    http_req_duration: ["p(99)<50"],   // p99 < 50ms
    http_req_failed:   ["rate<0.001"], // <0.1% errors
  },
};

let coid = 1_000_000;
export default function () {
  const body = JSON.stringify({
    client_order_id: __VU * 1_000_000 + coid++,
    account_id: __VU % 3 + 1,
    symbol: "AAPL",
    type: "LIMIT", side: __ITER % 2 ? "BID" : "ASK",
    price: 10000 + (__ITER % 5),
    qty: 10,
  });
  const res = http.post("http://127.0.0.1:8080/orders", body, {
    headers: { "Authorization": "Bearer dev-token", "Content-Type": "application/json" },
  });
  check(res, { "202": r => r.status === 202 });
  sleep(0.01);
}
```

Run:
```powershell
k6 run load/orders.js
```

### 4.6 Load testing WebSockets (k6)

```js
// load/ws.js
import ws from "k6/ws";
import { check } from "k6";

export const options = { vus: 200, duration: "1m" };

export default function () {
  const url = "ws://127.0.0.1:8080/ws";
  ws.connect(url, null, (socket) => {
    socket.on("open", () => {
      socket.send(JSON.stringify({type:"subscribe", channel:"trades", symbol:"AAPL"}));
    });
    let n = 0;
    socket.on("message", () => { n++; });
    socket.setTimeout(() => socket.close(), 30000);
    socket.on("close", () => check(n, { "got messages": v => v > 0 }));
  });
}
```

### 4.7 Stressing the C engine directly

Skip the API — measure raw match throughput:

```c
// load/engine_bench.c  (sketch)
uint64_t t0 = clock_gettime_ns();
for (int i = 0; i < 1<<20; i++) submit_random_order(eng);
uint64_t t1 = clock_gettime_ns();
printf("orders/sec = %.0f\n", (double)(1<<20) * 1e9 / (t1 - t0));
```

Run **with the matching engine pinned to a single core** (`taskset` on Linux,
`start /AFFINITY` on Windows). Compare before/after each refactor.

### 4.8 Stressing Postgres

```powershell
# pgbench: built-in, perfect for sanity checks
pgbench -i -s 10 profitplayground          # init (scale=10)
pgbench -c 50 -j 4 -T 60 profitplayground  # 50 clients, 60s
```

For our specific workload (heavy `order_events` inserts), write a custom script
mimicking 10k orders/s and watch `pg_stat_statements`.

### 4.9 Performance budgets

Pin numbers so you can detect regressions:

| Metric                          | Budget    | Measured |
|---------------------------------|-----------|----------|
| API order ack p99               | < 5 ms    | TBD       |
| C match p99 (single-symbol)     | < 5 µs    | TBD       |
| WS broadcast fan-out p99 (1k)   | < 10 ms   | TBD       |
| DB `order_events` insert p99    | < 1 ms    | TBD       |
| Sustained POST /orders          | > 5k rps  | TBD       |

Update the "Measured" column after each load run; treat regressions as bugs.

---

## 5. Operations basics (must-have before going live)

- **Secrets**: never commit `.env`. Use the platform's secret store
  (Render/Fly/AWS Secrets Manager). `API_TOKEN` and `DATABASE_URL` are the only
  secrets today — rotate quarterly.
- **Backups**: nightly `pg_dump` of `profitplayground` to off-host storage. Test
  restore monthly. Don't trust any backup you haven't restored.
- **Migrations**: only via `uv run --project api alembic upgrade head` in
  release pipelines. **Never** rely on dev `create_all` in production.
- **Graceful shutdown**: lifespan already cancels the pump task and disposes the
  pool (`@c:\0.Coding_pt2\ProfitPlayground antigrav\api\main.py:99-105`). Verify SIGTERM is honoured by your container runtime.
- **Resource limits**: cap uvicorn workers, DB pool size, WS clients per
  process. Document the numbers here once tuned.

---

## 6. How we keep this doc fresh

1. **Every postmortem ends with a PR** that updates this file (new alert, new
   test, new budget). No exceptions.
2. **Every new metric or log field** is documented under §2 or §3 the same day
   it ships.
3. **Every load run** updates the "Measured" column in §4.9. Even if the result
   is bad — especially if it's bad.
4. **Quarterly**: re-read §1 and demote anything that's drifted (e.g. a metric
   broke, dashboard rotted) from ✅ to 🟡.
5. **Owner rotation**: whoever last edited this file is on the hook for the
   next pass. Keep the git blame visible.

---

## 7. Open items / roadmap (sorted by leverage)

1. **Add `/metrics` + minimal Prometheus stack** — unlocks every other monitoring
   item. Do this first.
2. **Structured logging + request-id middleware** — biggest debug-time win.
3. **k6 load test for `POST /orders`** — gives us our first real budget number.
4. **`pg_stat_statements` baseline + dashboard.**
5. **Pytest harness with testcontainers Postgres.**
6. **C engine perf bench (rdtsc)** + commit baseline numbers.
7. **Dockerfile + CI** — once 1–5 are in, packaging is trivial.
8. **OpenTelemetry tracing** (optional; revisit when team > 3 people).

When item 1 is shipped, mark it ✅ in §1, add the new row(s) it created (e.g.
"Grafana dashboard URL"), and move the unblocked items up the queue.

---

*This file is the single source of truth for production posture. If reality
diverges from this document, the document is wrong — fix it.*
