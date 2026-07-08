#ifndef BRIDGE_H
#define BRIDGE_H

/*
 * bridge.h — Flat, ctypes-friendly C API over the trade pipeline.
 *
 * This is the only header the Python side needs to mirror. Every struct
 * here is plain-old-data (fixed-width ints only, plus one static string
 * pointer in the ack) so ctypes.Structure definitions in engine_bridge.py
 * can match byte-for-byte.
 *
 * Threading model: the underlying pipeline (engine/ + pipeline/) is
 * single-threaded and not reentrant. Callers MUST serialize every call
 * into this API with a single lock — one call in flight at a time, across
 * ALL symbols — because bridge_poll_event's data is populated synchronously
 * inside bridge_submit/bridge_cancel via the pipeline's event bus.
 *
 * Multi-symbol note: the underlying portfolio_t is single-symbol by design
 * (see pipeline/portfolio.h). bridge_add_account sets cash/limits scoped to
 * ONE symbol's book only — it is not a shared cross-symbol wallet. The
 * Python side treats this engine purely as the matching + per-symbol
 * position source of truth, and aggregates cross-symbol cash/PnL itself
 * from the trade event stream.
 */

#include <stdint.h>
#include <stdbool.h>

#if defined(_WIN32)
  #define BRIDGE_API __declspec(dllexport)
#else
  #define BRIDGE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle for one symbol's pipeline instance. */
typedef void *bridge_symbol_t;

/* ─── Request: mirrors gw_raw_msg_t, enums passed as plain ints ─────────── */
typedef struct {
    int32_t  action;          /* 1=NEW 2=CANCEL 3=MODIFY (gw_action_t)      */
    uint64_t client_order_id; /* 0 = no idempotency key (used for cancels)  */
    uint32_t account_id;
    int32_t  type;            /* me_order_type_t                            */
    int32_t  side;            /* ob_side_t: 0=BID 1=ASK                     */
    int64_t  price;
    int64_t  trigger_price;
    uint64_t qty;
    uint64_t ttl_ns;
    uint64_t target_order_id; /* CANCEL/MODIFY                              */
    int64_t  new_price;       /* MODIFY                                     */
    uint64_t new_qty;         /* MODIFY                                     */
} bridge_raw_msg_t;

/* ─── Ack: mirrors gw_ack_t ──────────────────────────────────────────────── */
typedef struct {
    int32_t     status;          /* gw_ack_status_t                        */
    uint64_t    server_order_id;
    uint64_t    ingress_ts_ns;
    uint64_t    ack_ts_ns;
    const char *reject_reason;   /* NULL or a static string literal        */
} bridge_ack_t;

/* ─── Derived order status (superset used for OrderView.status) ─────────── */
enum {
    BRIDGE_STATUS_OPEN      = 0,
    BRIDGE_STATUS_PARTIAL   = 1,
    BRIDGE_STATUS_FILLED    = 2,
    BRIDGE_STATUS_CANCELLED = 3,
    BRIDGE_STATUS_REJECTED  = 4,
};

typedef struct {
    uint64_t server_order_id;
    uint64_t client_order_id;
    uint32_t account_id;
    int32_t  type;
    int32_t  side;
    int64_t  price;
    int64_t  trigger_price;
    uint64_t qty_original;
    uint64_t qty_remain;
    uint64_t qty_filled;
    int32_t  status;
    uint64_t created_ts_ns;
    uint64_t updated_ts_ns;
    bool     found;
} bridge_order_view_t;

typedef struct {
    int64_t bid_price, bid_qty, ask_price, ask_qty, spread, mid;
} bridge_tob_t;

/* ─── Streamed event (order state change or trade) ───────────────────────── */
enum { BRIDGE_EVENT_ORDER = 0, BRIDGE_EVENT_TRADE = 1 };

typedef struct {
    int32_t  kind;
    uint64_t seq;
    uint64_t ts_ns;

    /* populated when kind == BRIDGE_EVENT_ORDER */
    uint64_t order_id;
    uint32_t account_id;
    int32_t  side;
    int32_t  type;
    int64_t  price;
    uint64_t qty_original, qty_remain, qty_filled;
    int32_t  status;

    /* populated when kind == BRIDGE_EVENT_TRADE */
    uint64_t trade_id;
    uint64_t aggressor_id, resting_id;
    uint32_t aggressor_account_id, resting_account_id;
    int32_t  aggressor_side;
    int64_t  trade_price;
    uint64_t trade_qty;
} bridge_event_t;

/* ─── Lifecycle ──────────────────────────────────────────────────────────── */

/* Create (or fetch, if already created) the pipeline for `symbol`.
 * Returns NULL if the symbol table is full or the name is too long. */
BRIDGE_API bridge_symbol_t bridge_add_symbol(const char *symbol);

/* Configure an account's starting cash + risk limits on one symbol's book.
 * See the multi-symbol note above: not a shared cross-symbol wallet. */
BRIDGE_API bool bridge_add_account(bridge_symbol_t h, uint32_t account_id,
                                   int64_t cash, int64_t max_position,
                                   int64_t max_order_notional);

/* ─── Order entry ─────────────────────────────────────────────────────────── */

BRIDGE_API bridge_ack_t bridge_submit(bridge_symbol_t h,
                                      const bridge_raw_msg_t *msg);

/* Convenience wrapper around bridge_submit for GW_ACT_CANCEL. */
BRIDGE_API bridge_ack_t bridge_cancel(bridge_symbol_t h, uint64_t order_id,
                                      uint32_t account_id);

BRIDGE_API bridge_order_view_t bridge_get_order(bridge_symbol_t h,
                                                uint64_t order_id);

/* ─── Market data ─────────────────────────────────────────────────────────── */

BRIDGE_API bridge_tob_t bridge_top_of_book(bridge_symbol_t h);

/* Fills up to `depth` levels per side into caller-supplied arrays (each
 * must have room for `depth` int64_t entries). Returns levels written. */
BRIDGE_API uint32_t bridge_book_levels(bridge_symbol_t h, int32_t side,
                                       uint32_t depth,
                                       int64_t *prices_out,
                                       int64_t *qtys_out);

/* ─── Event stream (non-blocking pop) ────────────────────────────────────── */

/* Pop one queued event for this symbol. Returns false if none pending. */
BRIDGE_API bool bridge_poll_event(bridge_symbol_t h, bridge_event_t *out);

#ifdef __cplusplus
}
#endif
#endif /* BRIDGE_H */
