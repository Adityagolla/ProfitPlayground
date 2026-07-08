# Trade Lifecycle Pipeline — Explanation

This document explains the end-to-end pipeline I built on top of the existing
matching engine (`matching_engine.c`, `orderbook.c`, `orders.c`). It walks
through every new file, why it exists, and how data flows from a raw inbound
message all the way to portfolio updates and subscriber callbacks.

---

## 1. The Architecture I Built

```
   API / WS / FIX / Bot
            │
            ▼
   ┌────────────────────────────────────────────────────────┐
   │  ORDER INTAKE GATEWAY  (the central entry point)        │
   │   Stage A — Ingress Adapter   (cheap structural checks) │
   │   Stage B — Normalizer        (canonical event)         │
   │   Stage C — Admission Control (kill, rate, dedup, IDs)  │
   └─────────────────────────────┬──────────────────────────┘
                                 │  (out_ring: SPSC ring of order_event_t)
                                 ▼
                   ┌────────────────────────────┐
                   │  Validation                │   schema, type-specific
                   ├────────────────────────────┤
                   │  Risk / Pre-Trade          │   margin, fat-finger, tick
                   ├────────────────────────────┤
                   │  Sequencer + WAL           │   monotonic seq + journal
                   ├────────────────────────────┤
                   │  Router                    │   (1 ME for now; sharded later)
                   ├────────────────────────────┤
                   │  Matching Engine (C core)  │   me_submit_order / stop / cancel
                   ├────────────────────────────┤
                   │  Trade Execution           │   drain ME events
                   ├────────────────────────────┤
                   │  Portfolio / State Update  │   per-account cash, P&L
                   ├────────────────────────────┤
                   │  Event Bus                 │   topic-mask pub/sub
                   └─────────────┬──────────────┘
                                 ▼
                ┌────────┬────────┬─────────────┐
                ▼        ▼        ▼             ▼
              UI     Analytics    ML       Outbound Gw
```

The gateway is **not a single function** — it's a 3-stage component with a
strict contract. Everything downstream is wired by `pipeline.c`.

---

## 2. Files I Added

| File | Role |
|------|------|
| `gateway.h` / `gateway.c` | Order Intake Gateway (A/B/C stages + central entry function) |
| `risk.h` / `risk.c` | Pre-trade risk: tick size, fat-finger, portfolio reservation |
| `portfolio.h` / `portfolio.c` | Per-account positions, cash, realised + unrealised P&L |
| `event_bus.h` / `event_bus.c` | In-process pub/sub fan-out to subscribers |
| `pipeline.h` / `pipeline.c` | The orchestrator that wires every stage end-to-end |
| `pipeline_demo.c` | Runnable demo exercising every feature |
| `explaination.md` | This document |

I did **not** modify any existing engine file. The pipeline composes on top.

---

## 3. The Central Entry Function — `gateway_submit()`

This is the only function any caller (HTTP handler, WebSocket loop, FIX
session, internal bot) is allowed to call. Its contract:

1. **Synchronous ack, asynchronous execution.**
   It returns a `gw_ack_t` immediately with one of:
   `ACCEPTED`, `REJECTED`, `DUPLICATE`, `THROTTLED`, `KILLED`.
   It does **not** wait for the matching engine.
2. **Idempotent on `client_order_id`.**
   A second submission with the same `(account_id, client_order_id)` returns
   the original `server_order_id` with status `DUPLICATE` — no double-trade.
3. **Monotonic timestamps.**
   Uses `QueryPerformanceCounter` (Windows) / `CLOCK_MONOTONIC` (POSIX) — never
   wall clock.
4. **Bounded latency.**
   If the downstream ring is full, returns `THROTTLED` instead of blocking.
5. **No side effects on the book or portfolio.**
   That's the next stages' job.

The function runs three sub-stages internally:

### Stage A — Ingress Adapter (`stage_a_ingress`)
Cheap structural checks on the raw message: required fields, qty>0, valid
side/action. Anything heavier (margin, tick size) is *not* here — that's the
risk layer.

### Stage B — Normalizer (`stage_b_normalize`)
Pure transform: copy fields from `gw_raw_msg_t` to the canonical
`order_event_t`. Everything downstream consumes the canonical form, never the
raw wire message.

### Stage C — Admission Control (`stage_c_admit`)
Policy checks:
- **Kill-switch** — if engaged, every NEW order is rejected (cancels still flow).
- **Token-bucket rate limit** — per-gateway global limit; trivial to make per-account.
- **Idempotency dedup** — open-addressed hash map, fixed capacity, no allocs.
- **ID + timestamp assignment** — the `server_order_id` is monotonic and
  globally unique.

Only after Stage C succeeds is the event pushed into `out_ring` for the next
pipeline stage.

---

## 4. The Pipeline Orchestrator (`pipeline.c`)

`pipeline_step()` is the consumer side: it pops the next event from the
gateway's out_ring and walks it through every remaining stage.

I deliberately kept the wiring **explicit** — each stage is a small static
function called in order — instead of generic stage callbacks. This makes the
data flow trivial to read and debug.

### Stage: Validation (`stage_validate`)
Type-specific deep checks:
- LIMIT/IOC/FOK require `price > 0`
- MARKET requires `price == 0`
- STOP requires `trigger_price > 0` and `price == 0`
- STOP_LIMIT requires `trigger_price > 0` and `price > 0`

### Stage: Risk (`risk_check` in `risk.c`)
- **Tick size** — limit price must be on the price grid.
- **Fat-finger** — limit price must be within ±10% (configurable bps band)
  of the last trade price.
- **Portfolio reservation** — calls `portfolio_reserve()` to lock notional /
  position headroom. If it would breach `max_position` or
  `max_order_notional`, the order is rejected here.

### Stage: Sequencer + WAL (`stage_sequence`)
- Assigns a strict monotonic `seq_no` (the global ordering of every event
  that will reach the matching engine).
- If a WAL is open, appends a one-line CSV record before matching.
- This is the LMAX pattern: **deterministic replay**. Re-feed the WAL into a
  fresh engine and you reproduce the exact same book state. Critical for
  audit, debugging, and crash recovery.

### Stage: Router + Matching (`match_new`, `match_cancel`)
Single-engine for now, but the dispatch is centralised:
- `STOP` / `STOP_LIMIT` → `me_submit_stop()`
- everything else → `me_submit_order()`
- cancel → `me_cancel_order()` then `me_cancel_stop()` fallback

To add per-symbol shards later, change only this function.

### Stage: Trade Execution (`drain_engine_events`)
After each match, the ME's `eq` ring contains zero or more events:
`ORDER_ACCEPTED`, `TRADE`, `ORDER_FILLED`, `BOOK_UPDATED`, `STOP_TRIGGERED`,
etc. We pop them and:
- For every `TRADE`: apply the fill to **both legs'** portfolios using the
  `id_map` (server_order_id → account_id). Update risk's reference price.
- For every event: build a `bus_msg_t` and publish to the bus.

### Stage: Portfolio Update (`portfolio_on_fill`)
- Updates cash (bid spends, ask receives).
- VWAP-updates `avg_price` when the fill extends the position.
- Realises P&L on the closed slice when the fill reduces or flips the
  position.
- Handles position flips correctly (closes old at avg, opens remainder at
  trade price).

### Stage: Event Bus (`bus_publish`)
- Topic-bitmask fan-out: `BUS_TOPIC_ORDER`, `TRADE`, `BOOK`, `PORTFOLIO`,
  `GATEWAY`.
- Subscribers register with `pipeline_subscribe(mask, handler, ctx)`.
- Synchronous dispatch in this build; production would add per-subscriber
  rings + worker threads so a slow ML consumer can't backpressure matching.

---

## 5. SPSC Ring Buffers

Every stage transition uses a `gw_ring_t` (or the existing
`me_event_queue_t`). They are:

- Power-of-two capacity → bitmask wrap.
- Single-producer, single-consumer in shape (head/tail not yet `_Atomic` —
  ready for that upgrade with no API change).
- Allocation-free, fixed capacity.
- Backpressure surfaces as `GW_ACK_THROTTLED` — the system never blocks.

This is the key to the "non-blocking" requirement: the gateway pushes into
a ring and returns immediately.

---

## 6. Why Each Layer Exists (the boundaries)

I drew the layer lines deliberately:

| Layer | Owns | Does NOT own |
|---|---|---|
| **Ingress Adapter** | wire decoding, auth | business rules |
| **Normalizer** | canonical form | book state |
| **Admission Control** | kill-switch, rate limit, dedup, IDs, timestamps | margin, tick rules |
| **Validation** | schema, type-specific checks | portfolio state |
| **Risk** | margin, fat-finger, tick, portfolio reservation | matching |
| **Sequencer/WAL** | strict ordering, durability | matching, P&L |
| **Router** | venue/shard selection | matching |
| **Matching Engine** | price-time priority, IOC/FOK/Stop | portfolio |
| **Trade Execution** | apply fills, attribute accounts, build bus msgs | matching |
| **Portfolio** | cash, position, P&L | events |
| **Event Bus** | fan-out, ordering | business logic |

This is what makes the system maintainable — each layer can be replaced
independently.

---

## 7. What the Demo Proves

Running `pipeline_demo.exe` exercises:

1. **LIMIT** — maker rests an ask, book updates.
2. **MARKET** — taker hits, trade event published.
3. **IOC** — partial fill, remainder cancelled.
4. **FOK** — rejected when liquidity insufficient.
5. **Idempotency** — duplicate `client_order_id` returns original ack.
6. **STOP** — sell stop triggers when price hits 9500, re-enters as market.
7. **Fat-finger guard** — limit at 99999 (vs last 9500) is rejected by risk.
8. **Kill-switch** — gateway returns `GW_ACK_KILLED` for new orders.
9. **Portfolio P&L** — both accounts have correct cash + position deltas
   after every trade.

Sample output (excerpt):

```
=== 2. Taker (acct=2) MARKET BUY 30 ===
  [GW] ACCEPTED  server_id=2  reason=-
  [UI] TRADE  trade_id=1  price=10000  qty=30  acct=2
  [UI] FILLED  id=2 side=0 qty=30->0 filled=30
  [UI] BOOK   bid=0(0)  ask=10000(70)
```

Final portfolios show realised cash flow + open positions, confirming the
trade execution and portfolio layers wired correctly.

---

## 8. How to Build & Run

From the project root (MinGW / GCC on Windows):

```powershell
gcc -O2 -Wall -Wno-unused-function -o pipeline_demo.exe `
    pipeline_demo.c pipeline.c gateway.c risk.c portfolio.c `
    event_bus.c matching_engine.c orders.c orderbook.c

.\pipeline_demo.exe
```

A `pipeline.wal` file is written alongside as the journal.

---

## 9. What I Did NOT Change

- `matching_engine.c` / `matching_engine.h` — untouched.
- `orderbook.c` / `orderbook.h` — untouched.
- `orders.c` / `orders.h` — untouched.

The only "central entry function" reshaping happens **above** the existing
engine. The engine keeps its own internal API (`me_submit_order`,
`me_submit_stop`, `me_cancel_order`) — the pipeline is just the layer that
makes sure those are only ever called through one disciplined path.

---

## 10. Future Upgrades (mapped to your earlier wishlist)

- Make `gw_ring_t` head/tail `_Atomic uint32_t` and run each stage on its own
  pinned thread. The struct layout is already cache-friendly.
- Replace the linear `id_map` lookup with a real hash map (account
  attribution).
- Per-account rate limiter buckets (currently one global bucket).
- Per-subscriber ring + worker thread on the event bus.
- WAL → snapshots: periodically dump engine state so replay doesn't have to
  start from t=0.
- MODIFY action: implement as cancel + re-add inside the router stage.
- Outbound gateway symmetric to ingress (FIX/WS encoders subscribed to
  bus topics).
