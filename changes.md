# Changes — Order Book & Matching Engine Refactor

## Overview

This document covers two phases of changes made to the passive limit order book (`orderbook.h/.c`) and the addition/integration of a full matching engine (`matching_engine.h/.c`). Every change is listed with what it was, what it replaced, and why it was made.

---

## Phase 1 — Order Book Refactor (`orderbook.h` / `orderbook.c`)

### 1. Preallocated Memory Pools (Zero `malloc` on Hot Path)

**What changed:**
- Removed all `malloc()`/`free()` calls from `ob_add_order`, `ob_cancel_order`, and `ob_modify_order`.
- Added two fixed-size pools directly embedded inside `ob_book_t`:
  - `order_pool[OB_MAX_ORDERS]` — holds all live order structs (4096 slots)
  - `level_pool[OB_MAX_LEVELS * OB_SIDE_COUNT]` — holds all price level structs (512 slots)
- Added free-stack arrays (`order_free_stack`, `level_free_stack`) and stack pointers (`order_free_top`, `level_free_top`) to track available slots.
- Implemented `order_pool_alloc()`, `order_pool_free()`, `level_pool_alloc()`, `level_pool_free()` as O(1) stack pop/push operations.

**Why:**
- Every `malloc()`/`free()` call goes through the OS allocator, which involves a system call, lock acquisition, and unpredictable latency.
- In a matching engine, `ob_add_order` is on the hot path — called thousands of times per second. Heap allocation adds 50–500 ns of jitter per call.
- With preallocated pools, allocation is a single array index decrement — deterministic O(1) with no system calls.

---

### 2. Cache-Friendly Index-Based Data Structures

**What changed:**

**`ob_order_t` (72B → 56B):**
- Replaced 8-byte `ob_order_t *prev`, `*next` FIFO pointers with `uint16_t prev_idx`, `next_idx` (pool indices).
- Reordered fields: hot fields (`id`, `price`, `qty_remain`, `side`) moved to the first 32 bytes so they fit in a single cache line.
- Cold fields (`qty`, `ts_submit`, `ts_update`) moved to the second half.

**`ob_level_t` (72B → 32B):**
- Replaced 8-byte `ob_level_t *left`, `*right`, `*head`, `*tail` pointers (4 × 8 = 32B of pointers) with `uint16_t left_idx`, `right_idx`, `head_idx`, `tail_idx` (4 × 2 = 8B of indices).
- This cut the struct size by more than half.

**`ob_side_t_s`:**
- Replaced `ob_level_t *root` with `uint16_t root_idx`.

**`ob_book_t` lookup table:**
- Stores `uint16_t pool_idx` instead of `ob_order_t *` — halves pointer storage in the hash table.

**Why:**
- Smaller structs mean more of them fit in a CPU cache line (64 bytes). When the matching loop walks a price level's FIFO queue, consecutive orders are closer together in memory, reducing cache misses.
- `uint16_t` indices (2 bytes) vs. 64-bit pointers (8 bytes) give a 4× size reduction per link field.
- Index-based structures are also more portable and easier to serialize/persist than raw pointers.
- The CPU hardware prefetcher works better on contiguous arrays than on pointer-chased linked lists scattered across the heap.

---

### 3. AVL Tree Converted to Index-Based

**What changed:**
- All AVL tree functions (`avl_insert`, `avl_remove`, `avl_find`, `avl_min`, `avl_max`, `avl_rotate_left`, `avl_rotate_right`) were rewritten to accept `ob_book_t *b` plus `uint16_t` root/node indices instead of `ob_level_t *` pointers.
- A helper macro `LV(book, i)` expands to `&book->level_pool[i]` for ergonomic access.
- `OB_NULL_IDX` (`UINT16_MAX`) serves as the null sentinel instead of `NULL`.

**Why:**
- Required by the pool design — there are no heap-allocated nodes to point to.
- Index arithmetic is faster than pointer chasing because the compiler can optimize array-base + offset into a single instruction.
- The AVL tree maintains price levels in sorted order (O(log n) insert/find/remove), which is necessary for best-bid/ask queries and matching traversal.

---

### 4. FIFO Queue Preserved Within Each Price Level

**What changed:**
- Each `ob_level_t` has a doubly-linked FIFO queue of orders, now tracked by `head_idx` (oldest, matched first) and `tail_idx` (newest, appended here).
- `level_enqueue()` appends to tail — O(1).
- `level_dequeue()` removes an arbitrary order from anywhere in the queue — O(1) using the doubly-linked `prev_idx`/`next_idx`.

**Why:**
- Exchange rules require Price-Time priority: among all orders at the same price, the one submitted earliest is filled first.
- FIFO at each level enforces this. Without it, newer orders could unfairly jump ahead of older ones at the same price.

---

### 5. Open-Addressed Hash Lookup Table

**What changed:**
- Added `lookup_table_t` — an open-addressed hash table with Fibonacci hashing, mapping `ob_order_id_t → uint16_t pool_idx`.
- Load factor ≤ 0.5 (table size = 2 × `OB_MAX_ORDERS`).
- Tombstone-free removal: deleted slots are backfilled by re-inserting displaced entries.

**Why:**
- `ob_cancel_order` and `ob_get_order` need O(1) lookup by order ID without scanning the entire book.
- Fibonacci hashing (`id × 11400714819323198485`) distributes keys uniformly across the table, minimising collision clusters.

---

### 6. Per-Book Read-Write Spinlock (Optional Threading)

**What changed:**
- Added `ob_rwlock_t` (an `_Atomic int32_t` spinlock) inside `ob_book_t`, guarded by `#ifdef OB_ENABLE_THREADS`.
- Added `OB_READ_LOCK/UNLOCK` and `OB_WRITE_LOCK/UNLOCK` macros that expand to spinlock operations when threading is enabled, or `((void)0)` when disabled.
- Semantics: `state == 0` = unlocked, `state == -1` = write-locked, `state > 0` = reader count.

**Why:**
- In a multi-threaded matching engine, multiple threads may read the book (market data) while one thread writes (order entry).
- A read-write lock allows concurrent readers, which is critical for throughput.
- Compile-time opt-in (`-DOB_ENABLE_THREADS`) means single-threaded builds pay zero cost.

---

### 7. Cross-Platform Timestamp (`now_ns`)

**What changed:**
- Replaced a bare `clock_gettime()` call with a `#ifdef _WIN32` branch:
  - Windows: `QueryPerformanceCounter` (high-resolution, no dependency on POSIX)
  - POSIX: `clock_gettime(CLOCK_MONOTONIC, ...)`

**Why:**
- The original code used `clock_gettime` which is a POSIX function not available on Windows without MinGW extensions.
- `QueryPerformanceCounter` is the Windows equivalent and provides sub-microsecond resolution.
- Without this fix, the code would not compile under MSVC or produce wrong timestamps on Windows.

---

### 8. `OB_STATUS_FILLED` and `OB_STATUS_PARTIAL` Added

**What changed:**
- Added two new values to `ob_status_t`:
  - `OB_STATUS_FILLED = 1` — aggressor order was completely consumed by matching.
  - `OB_STATUS_PARTIAL = 2` — aggressor was partially filled; remainder rests in the book.

**Why:**
- The matching engine needs to distinguish between three outcomes of `ob_add_order`:
  1. No fills — order just rests (`OB_STATUS_OK`)
  2. Partial fill — some qty matched, rest rests (`OB_STATUS_PARTIAL`)
  3. Full fill — aggressor gone, nothing rests (`OB_STATUS_FILLED`)
- Without these codes, the engine would have to re-query the book after every order submission to figure out what happened, adding latency and complexity.

---

### 9. `ob_match_t` Fill Record Added

**What changed:**
- New struct `ob_match_t` added to `orderbook.h`:
  ```c
  typedef struct {
      ob_order_id_t resting_id;  /* id of the resting order filled         */
      ob_price_t    price;       /* execution price (resting price)         */
      ob_qty_t      qty;         /* quantity filled                         */
      ob_ts_t       ts;          /* nanosecond timestamp of the fill        */
  } ob_match_t;
  ```
- `ob_add_order` now accepts a `ob_match_t *match_scratch` buffer and a `uint32_t *match_count` output parameter.

**Why:**
- The matching engine needs one fill record per matched resting order to:
  - Emit `ME_EVENT_TRADE` events for each fill.
  - Compute the volume-weighted average fill price (VWAP).
  - Attribute fills to the correct aggressor and resting order IDs.
- Without a scratch buffer, the engine would have to infer fills by comparing book state before and after — complex, slow, and race-prone.
- Caller-supplied scratch avoids any heap allocation per fill.

---

### 10. Matching Loop Added to `ob_add_order`

**What changed:**
- `ob_add_order` now calls `match_against_side()` before resting the order.
- `match_against_side()` walks the opposite side's AVL tree from best price, consuming FIFO queue head-first at each level until either the aggressor is fully filled or no more crossing levels exist.
- Fills are written into `match_scratch`. Fully-filled resting orders are dequeued and their pool slots are returned.
- Any unfilled remainder is rested in the aggressor's own side.

**Why:**
- Previously `ob_add_order` was purely passive — it only placed orders. The matching engine had no way to delegate matching to the book, so all fill logic would have had to live in the engine itself.
- Placing matching inside `ob_add_order` keeps the book self-consistent: all state changes (level qty, order removal, pool frees) happen atomically within the write lock, preventing races.
- The engine then only reads the scratch buffer — it never needs to re-enter the book to figure out what happened.

---

## Phase 2 — Matching Engine (`matching_engine.h` / `matching_engine.c`)

### 11. Fixed Include Path

**What changed:**
```c
// Before (broken):
#include "order_book.h"

// After (correct):
#include "orderbook.h"
```

**Why:**
- The header file is named `orderbook.h`, not `order_book.h`. This was a simple naming mistake that caused the engine to not compile at all.

---

### 12. `ob_match_t` Removed from Engine Header

**What changed:**
- Removed the duplicate `ob_match_t` definition from `matching_engine.h`.
- It now comes from `orderbook.h` (which the engine header includes).

**Why:**
- Having the same struct defined in two places causes a compilation error (redefinition). Since `ob_match_t` is produced by the book layer, it belongs in `orderbook.h`.

---

### 13. `match_scratch` Removed from `me_engine_t`

**What changed:**
```c
// Before — embedded in the struct:
ob_match_t match_scratch[OB_MAX_ORDERS];  // 4096 × 40B = ~160 KB

// After — stack-local inside me_submit_order():
ob_match_t match_scratch[OB_MAX_ORDERS];  // lives on the stack per call
```

**Why:**
- Embedding `match_scratch` permanently in `me_engine_t` adds ~160 KB to the engine struct.
- `me_engine_t` already contains `ob_book_t` (~255 KB). Adding another 160 KB makes the total ~415 KB — far too large to put on the stack, and wasteful as permanent heap memory since `match_scratch` is only needed during a single call.
- Stack-allocating it per call means it's only alive while `me_submit_order` is executing, and the compiler can keep it in registers/cache.

---

### 14. `fok_can_fill` Rewrote Tree Walk to Use Pool Indices

**What changed:**
```c
// Before (broken — using old pointer fields that no longer exist):
static void fok_sum_asc(ob_level_t *node, ...) {
    fok_sum_asc(node->left, ...);   // node->left is now gone
    fok_sum_asc(node->right, ...);
}

// After (correct — using pool index traversal):
static void fok_sum_asc(const ob_book_t *book, uint16_t node_idx, ...) {
    fok_sum_asc(book, FOK_LV(book, node_idx)->left_idx, ...);
    fok_sum_asc(book, FOK_LV(book, node_idx)->right_idx, ...);
}
```

**Why:**
- Phase 1 removed `ob_level_t *left` and `*right` pointer fields, replacing them with `uint16_t left_idx` and `right_idx` pool indices.
- The old `fok_can_fill` was still dereferencing the now-removed pointer fields, which would read garbage memory (undefined behaviour).
- The FOK pre-flight check must walk the book read-only to verify sufficient liquidity exists before committing any state changes. Getting this wrong means either accepting FOK orders that can't be fully filled, or rejecting ones that can.

---

### 15. Cross-Platform `me_now()` Timestamp

**What changed:**
```c
// Before (broken on Windows — POSIX only):
#include <time.h>
static ob_ts_t me_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);  // not available on Windows MSVC
    ...
}

// After (cross-platform):
#ifdef _WIN32
    // Uses QueryPerformanceCounter
#else
    // Uses clock_gettime(CLOCK_MONOTONIC, ...)
#endif
```

**Why:**
- `clock_gettime` is POSIX-only and not available in MSVC on Windows.
- The `orderbook.c` file already had this fix; `matching_engine.c` was using a bare `clock_gettime` which would fail to compile on Windows without MinGW's POSIX compatibility layer.
- Trade timestamps (`me_trade_t.ts`) are embedded in every fill record, so this affects correctness of every trade event.

---

### 16. `__USE_MINGW_ANSI_STDIO` Added to All Source Files

**What changed:**
- Added to `orderbook.c`, `matching_engine.c`, and `matching_engine_test.c`:
  ```c
  #if defined(__MINGW32__) || defined(__MINGW64__)
  #define __USE_MINGW_ANSI_STDIO 1
  #endif
  ```

**Why:**
- MinGW on Windows by default uses Microsoft's old MSVCRT for `printf`, which does not support C99 format specifiers like `%llu` (unsigned long long) or `%zu` (size_t).
- Without this define, `printf("%llu", value)` silently produces garbage output or compiler warnings.
- This define makes MinGW redirect `printf` to its own ANSI-compliant implementation.
- This affected every stats print function and every test assertion that printed order IDs or quantities.

---

### 17. `me_engine_t` Heap-Allocation Warning Added

**What changed:**
- Added a comment block to `me_engine_t` in `matching_engine.h`:
  ```c
  // NOTE: ob_book_t is ~255 KB (embedded pools). me_engine_t contains one,
  // so heap-allocate me_engine_t rather than placing it on the stack:
  //   me_engine_t *me = malloc(sizeof(me_engine_t));
  ```

**Why:**
- Default stack size on most systems is 1–8 MB. A `me_engine_t` on the stack would consume ~255 KB, leaving very little for the rest of the program.
- Without a warning, a developer writing `me_engine_t me;` as a local variable would get a silent stack overflow, which is extremely difficult to debug.

---

### 18. TOB Qty Bug Documented and Verified

**What changed:**
- Verified that `ob_depth(book, side, 0)` with `range = 0` correctly returns only the qty at the best price level.
- Added a comment explaining the semantics: `depth_in_tree` sums levels where `price >= best - range`, so `range = 0` gives exactly the best level.
- Fixed the conditional ordering: qty is now set only after confirming `bid_price > 0` / `ask_price > 0`.

**Why:**
- The original concern was that `ob_depth(..., 0)` always returned 0. After analysis, the issue was that the if-body was checking for non-zero price but was structured incorrectly in some paths.
- Correct TOB qty is essential for the `ME_EVENT_BOOK_UPDATED` event — market data consumers use it to display the best bid/ask with depth.

---

### 19. Order-Type Support in Matching Engine

**What was added:**

| Order Type | Behaviour |
|------------|-----------|
| `ME_ORDER_LIMIT` | Cross-checks against opposite side; fills at resting price; remainder rests if not crossed |
| `ME_ORDER_MARKET` | Assigned a sentinel price (INT64_MAX for bids, 1 for asks) so it crosses every level |
| `ME_ORDER_IOC` | Same as limit matching; any unfilled remainder is immediately cancelled (never rests) |
| `ME_ORDER_FOK` | Pre-flight check verifies full qty available; either fills completely or rejects entirely |

**Why:**
- These are the four most common order types on modern exchanges.
- Market orders provide execution certainty at the cost of price certainty.
- IOC is widely used by algorithmic traders who want fills but do not want open resting positions.
- FOK is used for atomic block trades where partial fills are unacceptable.

---

### 20. Event Ring Buffer

**What was added:**
- `me_event_queue_t` — a fixed-capacity ring buffer of `me_event_t` (1024 slots).
- Power-of-two capacity so head/tail can be masked with `& EQ_MASK` instead of expensive modulo.
- Events are pushed by `me_submit_order`, `me_cancel_order`, `me_modify_order`.
- Events are consumed by `me_run_events()` which calls the registered `me_handler_fn` callback.

**Why:**
- Decouples order entry from event processing — the engine doesn't block waiting for the handler to finish.
- Ring buffer with no heap allocation means event dispatch is deterministic and allocation-free.
- The power-of-two mask trick avoids integer division, which is important since this runs per-fill.

---

## Summary of Files Changed

| File | Changes |
|------|---------|
| `orderbook.h` | Added pools, indices, `ob_match_t`, `OB_STATUS_FILLED/PARTIAL`, updated `ob_add_order` signature |
| `orderbook.c` | Rewrote all data structures to use pool indices, added matching loop in `ob_add_order`, cross-platform timestamp, MinGW define |
| `matching_engine.h` | Fixed include path, removed `ob_match_t` duplicate, removed `match_scratch` from engine struct, added heap note |
| `matching_engine.c` | Cross-platform timestamp, pool-index FOK walk, stack-local scratch, MinGW define, full comments |
| `matching_engine_test.c` | Added MinGW define |
| `order_main.c` | Updated `ob_add_order` calls to pass `NULL, NULL` for new params |

---

## 2026-05-04: Decision Gate & Advanced Order Types

### 1. Decision Gate Architecture (`orders.c` extracted)
To decouple order validation and routing from the core matching logic, we extracted a "decision gate" into `orders.c` and `orders.h`. 
- **`orders_preprocess()`**: All orders flow through this single entry point. It validates parameters and determines the routing path (`FLOW_LIMIT`, `FLOW_MARKET`, `FLOW_IOC`, `FLOW_FOK`, `FLOW_STOP`, `FLOW_REJECT`).
- **FOK Optimisation**: FOK liquidity checks (`fok_sum_asc`/`fok_sum_desc`) were optimised with subtree pruning. The AVL walk now abandons subtrees that are guaranteed to be uncrossable based on the order's limit price, drastically reducing node visits.
- **Market Order Liquidity Guard**: Market orders now pre-check if the opposite book is completely empty. If so, they immediately reject instead of returning success with zero fills.

### 2. Stop and Stop-Limit Orders
Implemented dormant order states that do not rest in the order book until a price condition is met.
- **Stop Pool**: Added `me_stop_order_t` and a pre-allocated array of `stops[ME_MAX_STOPS]` directly inside `me_engine_t`. Stops are completely invisible to the `orderbook.c` layer.
- **Trigger Logic**: Industry standard implemented: stops trigger based on the **last trade price**. 
- **Event Flow**: After every fill, `emit_trade()` updates `last_trade_price` and calls `check_stops()`. If a stop's trigger price is crossed (e.g., `last_trade >= trigger` for a buy stop), it emits `ME_EVENT_STOP_TRIGGERED` and is immediately resubmitted to the engine as a live market or limit order.

### 3. Day Orders (Time-in-Force)
Implemented time-based expirations.
- **`expiry_ts`**: Expanded the core `ob_order_t` struct from 56 bytes to exactly 64 bytes (1 cache line) to hold an `expiry_ts` (in nanoseconds).
- **Sweep Mechanism**: Added `me_expire_orders(me, now)`, which walks the order pool and the dormant stop pool. Any resting order or dormant stop with an expiry timestamp older than `now` is cancelled, emitting an `ME_EVENT_ORDER_CANCELLED` event with the reason `"day order expired"`.

### Summary of Files Changed

| File | Changes |
|------|---------|
| `orderbook.h` | Added `expiry_ts` to `ob_order_t` (now 64B cache-line aligned), updated `ob_add_order` signature |
| `orderbook.c` | Store `expiry_ts` in `ob_add_order` |
| `matching_engine.h` | Added `ME_ORDER_STOP`, `ME_ORDER_STOP_LIMIT`, `ME_EVENT_STOP_TRIGGERED`, `me_stop_order_t`, stop pool inside engine |
| `matching_engine.c` | Added `check_stops()` hook after trades, `me_submit_stop`, `me_cancel_stop`, `me_expire_orders` |
| `orders.h` / `orders.c` | Created module. Added validation for stop params, `FLOW_STOP` routing |
| `matching_engine_test.c`| Added 5 new tests for stop trigger logic, dormant cancellation, and day order expiry |
| `order_main.c` | Updated `ob_add_order` test calls with the new `expiry_ts = 0` parameter |

---

## Build Instructions

```bash
# Full pipeline demo (Makefile)
make && make run

# Full pipeline demo (direct gcc)
gcc -O2 -Wall -Wno-unused-function -Iengine -Ipipeline -o demo/pipeline_demo.exe \
    demo/pipeline_demo.c \
    pipeline/gateway.c pipeline/risk.c pipeline/portfolio.c pipeline/event_bus.c pipeline/pipeline.c \
    engine/matching_engine.c engine/orders.c engine/orderbook.c && \
    ./demo/pipeline_demo.exe

# Matching engine full test suite (15 tests)
gcc -Wall -Wextra -O2 -Iengine -o me_test.exe \
    matching_engine_test.c engine/matching_engine.c engine/orderbook.c engine/orders.c && \
    ./me_test.exe

# Passive book smoke tests only
gcc -Wall -Wextra -O2 -Iengine -o ob_demo.exe order_main.c engine/orderbook.c && \
    ./ob_demo.exe

# With threading enabled
gcc -Wall -Wextra -O2 -DOB_ENABLE_THREADS -Iengine -o me_test.exe \
    matching_engine_test.c engine/matching_engine.c engine/orderbook.c engine/orders.c
```

---

*Last updated: 2026-05-05*

---

## 2026-05-05 — Engine/Pipeline Split & Real-Time Trade Pipeline

### Repository Reorganization

- Introduced module folders:
  - `engine/` — core order book + matching engine + decision gate
  - `pipeline/` — gateway, risk, portfolio, event bus, orchestrator
  - `demo/` — `pipeline_demo.c` (end-to-end run)
  - `docs/` — `explaination.md`
- Added a `Makefile` with `make` and `make run` for MinGW/GCC.

### New Modules (Public API Highlights)

- `pipeline/gateway.[ch]`
  - Central entry component with 3 sub-stages (Ingress, Normalizer, Admission)
  - `gw_ack_t gateway_submit(gateway_t *g, const gw_raw_msg_t *msg);`
  - SPSC ring (`gw_ring_t`) for non-blocking handoff; idempotency cache
- `pipeline/risk.[ch]`
  - Tick-size, fat-finger band (bps), portfolio reservation (position/notional)
- `pipeline/portfolio.[ch]`
  - Cash, net position, VWAP avg price, realised/unrealised P&L
- `pipeline/event_bus.[ch]`
  - Topic-bitmask pub/sub; synchronous fan-out (bus can be upgraded later)
- `pipeline/pipeline.[ch]`
  - Orchestrator wiring: Validation → Risk → Sequencer/WAL → Router → Matching → Trade Exec → Portfolio → Bus

### Demo

- `demo/pipeline_demo.c` exercises:
  - LIMIT, MARKET, IOC, FOK, STOP
  - Idempotency (duplicate `client_order_id`)
  - Risk (fat-finger) and kill-switch
  - Portfolio attribution for both legs and analytics counters

### Bug Fixes

- Gateway Stage A validation incorrectly rejected `ME_ORDER_LIMIT` because `type==0` was treated as "missing". Fixed to only reject unknown types (`> ME_ORDER_STOP_LIMIT`).

### Notes

- Matching engine and order book sources were not modified; the pipeline composes on top.

---

## Phase 4 — Wiring the FastAPI Layer to the Real C Engine

### 1. `bridge/libpipeline.dll` build

**What changed:**
- The Makefile's `dll` target (`CC64`) was pointed at the Anaconda
  `m2w64-toolchain` gcc 5.3, per its own comment. On this machine that
  compiler hits an internal compiler error (segfault) on any function
  returning/taking a `double` — reproduced with a two-line repro file, so
  it's not specific to this codebase.
- Default `CC64` changed to `zig cc` (the `ziglang` PyPI package, pulled
  on demand via `uv run --with ziglang`), targeting
  `x86_64-windows-gnu`. No system-wide toolchain install required; `make
  dll` now works out of the box wherever `uv` is available.
- `bridge/libpipeline.dll` builds clean and loads into the 64-bit
  `api/.venv` Python via `ctypes` (verified by loading it and resolving
  all 8 exported `bridge_*` symbols).

**Why:** the DLL is the only supported hookup point from Python into the
matching engine; the API can't drive real fills without it.

### 2. `pipeline/pipeline.c` — MODIFY was a TODO

**What changed:**
- `pipeline_step()` previously only handled `GW_ACT_NEW` and
  `GW_ACT_CANCEL`; `GW_ACT_MODIFY` fell through silently. Added a branch
  calling `me_modify_order()` (already implemented in the matching
  engine, just never wired up).

**Why:** `PATCH /orders/{id}` in the API needs a working modify path
against the real engine, not a client-side cancel+re-add simulation.

### 3. `engine/orderbook.c` — pool-free left stale order data behind

**What changed:**
- `order_pool_free()` returned a slot to the free-stack but left the old
  `id` and `expiry_ts` in place. `me_expire_orders()` sweeps the raw pool
  array for order slots whose `expiry_ts` has passed — with the fix, a
  freed slot could be picked up as if it were still a live day order.
- Fix: `order_pool_free()` now zeroes `id` and `expiry_ts` before
  releasing the slot.

**Why:** caught this via a bridge-level IOC test: an IOC order that
partially filled and had its remainder cancelled reused a pool slot
whose old `expiry_ts` from an earlier order was still set, corrupting the
CANCELLED order's reported state through the bridge. The existing 20 C
unit tests (`matching_engine_test.c`, `order_main.c`) don't exercise pool
reuse across cancel+resubmit in the same way the bridge's continuous
order flow does, so this had gone unnoticed.

### 4. `bridge/bridge.c` — id translation and event ordering

**What changed:**
- The gateway (exposed to Python) and the matching engine each assign
  their own order ids independently. `bridge_submit`'s pre-registration
  logic assumed they were the same number, which happens to hold for the
  first order but silently drifts once cancels/rejections cause the two
  counters to diverge. Added an explicit `me_id -> gateway_id` map
  (`bridge_pipeline_t.me_to_gw`) populated right after `pipeline_step()`
  from the pipeline's own `id_map`, and every subsequent CANCEL/MODIFY
  now translates the caller's gateway id to the ME id before calling into
  the engine.
- Bus events fire *during* `pipeline_step()`, before the me_id<->gw_id
  mapping for the just-submitted order exists yet. Fixed by staging raw
  `me_event_t`s during the step (`bridge_pipeline_t.staged[]`) and
  translating + publishing them only afterward
  (`process_staged_events()`), once the mapping is known.
- The matching engine never emits a status event for the resting
  (non-aggressor) side of a trade — only for the order that just crossed
  the book. `process_staged_events()` now derives the resting order's new
  qty_filled/qty_remain/status directly from the trade event and
  publishes a synthetic order snapshot for it, so both legs of a trade are
  observable through `bridge_poll_event`.
- Guarded against the ME's IOC follow-up event (a FILLED event fired
  right after the CANCELLED event when an IOC's unfilled remainder is
  cancelled) resurrecting an already-terminal order state.

**Why:** required to give the Python bridge (`engine_bridge.py`) a
correct, ordered view of every order and trade without reaching into
engine internals from Python.

### 5. `api/engine_bridge.py` — `_CEngine` (ctypes bridge)

**What changed:**
- Added `_CEngine`, a `ctypes`-based implementation of the same interface
  the existing in-memory mock (`_Engine`) already exposed to routes/WS
  (`submit_order`, `cancel_order`, `modify_order`, `get_order`,
  `top_of_book`, `book_snapshot`, `trades`, `portfolio`, `events`).
  `bridge.h`'s structs are mirrored field-for-field as `ctypes.Structure`
  subclasses.
- One C pipeline per symbol, created lazily on first order; accounts are
  lazily registered per-symbol the first time they trade (matches the
  fact that `portfolio_t` is single-symbol by design — see `bridge.h`'s
  own multi-symbol note).
- Cross-symbol trade tape and portfolio (cash/avg-price/realised PnL)
  aggregation live in Python, fed from `bridge_poll_event`, since the C
  side only tracks one symbol's book/portfolio per pipeline instance.
- All DLL calls run under one `asyncio.Lock` — the C pipeline is
  single-threaded and not reentrant (documented in `bridge.h`).
- Engine selection happens once at import (`_make_engine()`): if
  `settings.pipeline_dll` points at a file that exists, use `_CEngine`;
  otherwise fall back to the mock. Lets the API run before `make dll` has
  ever been run.
- `config.py` gained a `pipeline_dll` setting (env var `PIPELINE_DLL`,
  defaults to `<repo>/bridge/libpipeline.dll`).

**Why:** this was the last piece connecting the FastAPI surface to the
actual matching engine instead of the mock's simulated fills.

### 6. `api/subscriptions.py` — pre-existing WebSocket bug

**What changed:**
- `Client` was a plain `@dataclass` (auto-generates `__eq__`/`__hash__`
  from fields) but is stored in a `set[Client]`. Since `Client` holds a
  `list` and an `asyncio.Queue`, every instance was unhashable — every
  `/ws` connection crashed with `TypeError: unhashable type: 'Client'` on
  the very first `manager.add()`. Changed to `@dataclass(eq=False)` so
  `Client` uses identity-based hashing (correct — each connection is a
  distinct client regardless of field values).

**Why:** found while smoke-testing the live bridge end-to-end over
WebSocket; unrelated to the bridge itself but blocked verifying it.

### 7. `_CEngine` also emits orderbook deltas

**What changed:** after draining engine events for a symbol, if anything
was drained, push one `orderbook`/`delta` event with the current top-20
book levels — so `orderbook` WS subscribers see live updates without
polling, matching the mock engine's behavior via the REST snapshot path.

### Verification

- All 20 pre-existing C unit tests still pass after the `orderbook.c` /
  `pipeline.c` changes (`matching_engine_test.c` — 15/15,
  `order_main.c` — 5/5), plus the existing `pipeline_demo.exe` run.
- New direct Python test (`_CEngine` against the DLL, no HTTP) covering:
  resting order, idempotent duplicate detection, partial fill, trade
  tape, book snapshot, portfolio cash/position/PnL for both legs,
  modify, cancel, double-cancel, unknown-order lookup, multi-symbol id
  namespacing, and IOC-with-no-liquidity cancellation.
- End-to-end smoke test through the running FastAPI server (real DLL,
  real Postgres): `POST /orders` (resting ASK + crossing BID) →
  `GET /orderbook/{symbol}/top`, `GET /trades`, `GET /orders/{id}`,
  `GET /portfolio/{id}` all reflect the real engine's fill; DB rows
  appear in `orders`, `order_events`, `trades`, `portfolio_ledger`; and a
  WebSocket client subscribed to `orderbook`/`trades` receives the
  `snapshot`, `delta`, and `trade` events for the same order flow.

---

## Phase 5 — Frontend (2-Player Trading Dashboard)

### 1. New `frontend/` app

**What changed:** added a Vite + React + TypeScript app with no chart
library dependency — the price chart is a small dependency-free `<canvas>`
component (`src/components/PriceChart.tsx`) fed directly by the live
trade tape, kept intentionally minimal so the bundle and updates stay
fast for two players trading against each other in real time.

- `src/types.ts` mirrors `api/schemas.py` field-for-field.
- `src/api.ts` is a thin REST client (bearer token from `VITE_API_TOKEN`,
  default `dev-token`).
- `src/hooks/useEngineSocket.ts` owns one `/ws` connection per
  `(symbol, accountId)` pair: authenticates, subscribes to
  `orderbook`/`trades`/`portfolio`/`orders`, reconnects with exponential
  backoff, and hydrates once via REST on mount since the `portfolio` and
  `orders` WS channels only push on new events (no subscribe-time
  snapshot, unlike `orderbook`).
- Private `orders` events aren't scoped to one account server-side, so
  the hook filters them client-side to the active `accountId`.
- Vite's dev server proxies `/api/*` and `/ws` to `127.0.0.1:8080`
  (`vite.config.ts`), so there's no CORS setup and no `.env` needed to
  get started.

### 2. Layout — combining the two reference screenshots

**What changed:** built a CSS Grid shell (`grid-template-areas` in
`src/styles.css`) combining NAGA's structured panel layout (top stats
bar, big chart, right-side order ticket with a price/qty stepper) with
the second reference's dark theme and left icon rail. Below ~980px the
grid collapses to a single column (ticket → chart → ladder → tape →
positions) via one `@media` block.

### 3. Bug: mobile rail-hide rule lost the cascade

**What changed:** the responsive `@media (max-width: 980px) { .rail
{ display: none; } }` override was originally placed right after
`.app-shell`, before the base `.rail { display: flex; ... }` rule later
in the file. At equal specificity, CSS cascade order — not media-query
nesting — decides the winner when both rules' conditions are satisfied,
so the later unconditional rule always won and the rail never actually
hid on narrow viewports. Fixed by moving the whole responsive block to
the end of the stylesheet, after every other rule it needs to override,
with a comment explaining why the position matters.

**Why:** caught by checking `getComputedStyle(...).display` at a mobile
viewport width rather than trusting a screenshot — the Preview tool's
screenshot capture had its own unrelated rendering-size quirk in this
environment (confirmed via `getBoundingClientRect()`/computed-style
checks that the real DOM layout was correct at every viewport size
tested), so screenshots alone weren't a reliable signal here.

### Verification

- `npx tsc -b --noEmit` — clean, no type errors.
- Live-server checks (DOM geometry, computed styles, accessibility
  snapshot, network trace) against the running API confirm: correct
  4-column desktop grid (rail/ladder/chart/ticket) and single-column
  mobile stack with the rail hidden; REST hydration on load
  (`/api/orderbook`, `/api/trades`, `/api/portfolio`); a real order
  submitted through the Buy/Sell ticket UI got `ACCEPTED` from the live
  C engine and the new resting price level appeared in the ladder via
  the WebSocket `delta` event, with no manual refetch.
- `matching_engine_test.c` and `order_main.c` remain at repo root; use `-Iengine` include path when compiling.

---

## Phase 6 — Face-Off Mode: Rooms, Live Prices, Market-Maker Bots

FIFA-style stock duel. Each player picks an instrument from a roster,
gets identical starting cash ($100k), and trades its live market price in
their own order book for a fixed match length. Higher net P&L at full
time wins. Players never share a book — the comparison is the controlled
experiment, not the counterparty.

### Backend

- `api/rooms.py` — in-process room registry + REST routes:
  `POST /rooms` (create, returns 5-char code + private player token),
  `POST /rooms/{code}/join`, `POST /rooms/{code}/pick`,
  `GET /rooms/{code}` (public lobby state), `GET /rooms/{code}/me`
  (session restore), `GET /rooms/{code}/score` (live scoreboard + winner),
  `GET /instruments` (roster with cached live quotes).
  Room lifecycle: waiting → picking → live → finished. Each player gets a
  room-scoped engine symbol ("<code><pid>:<instr>", fits
  OB_MAX_SYMBOL_LEN) and 2 engine accounts (self + bot) from a global
  counter. Match end is lazy (`finish_if_due`) + bots stop on the clock.
- `api/instruments.py` — curated roster: crypto via Binance public API
  (keyless, 24/7: BTC/ETH/SOL in sub-unit lots so one lot stays
  game-priced) and 2025 top stocks + banks via Finnhub
  (`FINNHUB_API_KEY`, hidden without a key). Fixed-point conversion
  (`to_engine_price`) and a 10s TTL quote cache for the pick screen.
- `api/market_maker.py` — one bot task per player book: polls the live
  price (1.5s crypto / 5s stocks), cancels its two stale quotes, and
  re-quotes bid/ask at ±5bps around live mid, 50 lots a side, through the
  same engine bridge as the REST layer (deep-pocket account, cash 10^14).
- `api/auth.py` — rewritten around `Principal` (admin token or room
  player token). `require_principal` on orders/portfolio; player identity
  is server-enforced: `POST /orders` overwrites `account_id` and `symbol`
  from the token, order reads/cancels/modifies 404 across players,
  portfolio is 403 across players, and orders are 409 unless the match is
  live. WS auth accepts player tokens and derives `user_id` server-side;
  `subscriptions._matches` hard-filters private channels by that identity.
- `engine_bridge.py` — public `ensure_account(symbol, account, cash, …)`
  on both engines (players get instrument cash, bots get deep pockets;
  Python portfolio mirror seeded with the same figure). Order WS
  envelopes now carry `user_id` for the private-channel filter.
- `pipeline.h` — `PIPELINE_MAX_ACCOUNTS` 16 → 64 (each room consumes 4
  ids); DLL rebuilt, all 20 C tests + bridge tests still green.

### Frontend

- `Lobby.tsx` — create match (5/10/15 min) or join by code.
- `PickScreen.tsx` — roster cards with live prices + daily change,
  pick-state badges for both players, copyable room code.
- `TopBar.tsx` — rewritten as the match header: FIFA-style scoreline
  (You vs Rival net P&L, leader highlighted), 1s countdown clock, cash /
  position / live-price stats. Symbol tabs and the honor-system player
  switcher are gone — identity comes from the room token.
- `ResultOverlay.tsx` — full-time screen: winner/draw headline, both
  P&Ls, back-to-lobby.
- `App.tsx` — session state machine (localStorage `pp.session`, restored
  via `/rooms/{code}/me` on reload): Lobby → PickScreen (2s room poll) →
  match grid (2s score poll) → ResultOverlay. Engine socket + REST auth
  switch to the player token (`setAuthToken`).
- `useEngineSocket.ts` — skips connecting outside a match; re-hydrates
  the portfolio via REST on every own-order event (the C engine emits no
  portfolio WS events, so fills would otherwise leave cash/positions
  stale — found live when the positions panel stayed empty after a fill).
- `PositionsPanel.tsx` — strips the room prefix from engine symbols.

### Verified

- 18-check backend script: roster quotes, create/join, pre-live order
  gating (409), picks → live, player-scoped books, bot quotes within
  0.1% of live BTC, market order fills against the bot, score reflects
  position/cash/mark, cross-player portfolio 403 / cancel 404.
- Browser E2E (Vite preview + real API + real Binance feed): create room
  in UI → join+pick via API as rival → match screen with ticking clock,
  live bot ladder, market buy from the ticket → tape + score + positions
  all updated; mobile (375px) stacks to a single column.
