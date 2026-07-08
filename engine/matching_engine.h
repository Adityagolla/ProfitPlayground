#ifndef MATCHING_ENGINE_H
#define MATCHING_ENGINE_H

/*
 * matching_engine.h — Core matching engine built on top of ob_book_t
 *
 * Responsibilities
 * ----------------
 *  1. Accept incoming orders (Market, Limit, IOC, FOK)
 *  2. Determine if a cross exists (buy_price >= best_ask, sell_price <= best_bid)
 *  3. Execute fills using Price-Time priority (best price → earliest order)
 *  4. Emit structured events for every state change
 *  5. Run an event loop that dispatches events to registered handlers
 *
 * Architecture
 * ------------
 *
 *   [Incoming Order]
 *        │
 *        ▼
 *   me_submit_order()
 *        │
 *        ├─► [Market Order]  → match_market()
 *        │                        └─► consume best levels until filled / book empty
 *        │
 *        └─► [Limit Order]   → cross check
 *                 ├─► crossed  → match_limit() → partial fill → rest remainder
 *                 └─► not crossed → rest full qty in book
 *        │
 *        ▼
 *   Event queue  (me_event_t ring buffer)
 *        │
 *        ▼
 *   me_run_events()  → dispatches to me_handler_fn callbacks
 *
 * Future upgrades
 * ---------------
 *  - Lock-free SPSC ring buffer for the event queue (replace mutex)
 *  - Separate order-entry thread and matching thread
 *  - Batch event dispatch (SIMD memcpy of event structs)
 *  - Stop order / iceberg / peg order types
 */

#include "orderbook.h"     /* ob_book_t, ob_match_t, all book types          */
#include <stdint.h>
#include <stdbool.h>

/* ─── Order types ───────────────────────────────────────────────────────── */

typedef enum {
    ME_ORDER_LIMIT      = 0,  /* rest in book if not immediately matchable       */
    ME_ORDER_MARKET     = 1,  /* fill at any price, no resting                   */
    ME_ORDER_IOC        = 2,  /* Immediate-Or-Cancel: fill what you can, cancel rest */
    ME_ORDER_FOK        = 3,  /* Fill-Or-Kill: fill 100% instantly or reject all  */
    ME_ORDER_STOP       = 4,  /* dormant until trigger price hit, then market     */
    ME_ORDER_STOP_LIMIT = 5,  /* dormant until trigger price hit, then limit      */
} me_order_type_t;

/* ─── Event types ───────────────────────────────────────────────────────── */

typedef enum {
    ME_EVENT_ORDER_ACCEPTED  = 1,  /* new order entered the book              */
    ME_EVENT_ORDER_REJECTED  = 2,  /* order failed validation                 */
    ME_EVENT_ORDER_FILLED    = 3,  /* order completely filled                 */
    ME_EVENT_ORDER_PARTIAL   = 4,  /* order partially filled, still resting   */
    ME_EVENT_ORDER_CANCELLED = 5,  /* order removed from book                 */
    ME_EVENT_ORDER_MODIFIED  = 6,  /* order price/qty changed                 */
    ME_EVENT_TRADE           = 7,  /* a fill between aggressor and resting    */
    ME_EVENT_BOOK_UPDATED    = 8,  /* best bid or ask changed                 */
    ME_EVENT_STOP_TRIGGERED  = 9,  /* stop order activated by trade price     */
} me_event_type_t;

/* ─── Trade record (one fill event) ───────────────────────────────────────
 *
 * Produced for every aggressor↔resting match.
 * This is what gets published to the tape / feed.
 */
typedef struct {
    ob_order_id_t  trade_id;       /* monotonic trade sequence number         */
    ob_order_id_t  aggressor_id;   /* order that triggered the match          */
    ob_order_id_t  resting_id;     /* passive order that was sitting in book  */
    ob_side_t      aggressor_side; /* which side the aggressor was on         */
    ob_price_t     price;          /* execution price (resting order's price) */
    ob_qty_t       qty;            /* filled quantity                         */
    ob_ts_t        ts;             /* nanosecond timestamp                    */
} me_trade_t;

/* ─── Book top-of-book snapshot ─────────────────────────────────────────── */

typedef struct {
    ob_price_t bid_price;   /* best bid price (0 if empty)                   */
    ob_qty_t   bid_qty;     /* total qty at best bid                         */
    ob_price_t ask_price;   /* best ask price (0 if empty)                   */
    ob_qty_t   ask_qty;     /* total qty at best ask                         */
    ob_price_t spread;      /* ask - bid (−1 if either side empty)           */
    ob_price_t mid;         /* (bid + ask) / 2                               */
} me_top_of_book_t;

/* ─── Engine event (union of all event payloads) ─────────────────────────
 *
 * One flat struct — no heap allocation per event.
 * FUTURE: store in a preallocated ring buffer (power-of-two size,
 * head/tail as atomic uint32_t) for lock-free SPSC.
 */
typedef struct {
    me_event_type_t  type;
    ob_ts_t          ts;            /* when the event was generated           */
    ob_order_id_t    order_id;      /* primary order involved                 */
    ob_side_t        side;          /* bid or ask                             */
    me_order_type_t  order_type;    /* limit / market / ioc / fok             */
    ob_price_t       price;         /* limit price (0 for market)             */
    ob_qty_t         qty_original;  /* original quantity submitted            */
    ob_qty_t         qty_remain;    /* remaining after any fills              */
    ob_qty_t         qty_filled;    /* how much was filled this event         */

    /* Populated for ME_EVENT_TRADE */
    me_trade_t       trade;

    /* Populated for ME_EVENT_BOOK_UPDATED */
    me_top_of_book_t tob;

    /* Human-readable reason for ME_EVENT_ORDER_REJECTED */
    char             reject_reason[48];
} me_event_t;

/* ─── Event handler callback ─────────────────────────────────────────────
 *
 * Registered by the caller. Called synchronously from me_run_events().
 * FUTURE: dispatch on a separate consumer thread; callback becomes a
 * lock-free dequeue from the SPSC ring.
 */
typedef void (*me_handler_fn)(const me_event_t *event, void *user_ctx);

/* ─── Event queue ────────────────────────────────────────────────────────
 *
 * Simple fixed-capacity ring buffer.
 * FUTURE: replace head/tail with _Atomic uint32_t for lock-free SPSC.
 */
#define ME_EVENT_QUEUE_CAP  1024   /* must be power of two for lock-free     */

typedef struct {
    me_event_t  events[ME_EVENT_QUEUE_CAP];
    uint32_t    head;              /* producer writes here                    */
    uint32_t    tail;              /* consumer reads here                     */
    uint32_t    dropped;           /* events lost due to full queue           */
} me_event_queue_t;

/* ─── Stop order (dormant, not in the book) ─────────────────────────────── */

#define ME_MAX_STOPS 256

typedef struct {
    ob_order_id_t   id;             /* assigned at submission                 */
    ob_side_t       side;
    me_order_type_t orig_type;      /* STOP or STOP_LIMIT                    */
    ob_price_t      trigger_price;  /* activates when last trade crosses this */
    ob_price_t      limit_price;    /* 0 for stop-market, >0 for stop-limit  */
    ob_qty_t        qty;
    ob_ts_t         expiry_ts;      /* 0 = GTC (no expiry)                   */
    bool            active;         /* slot in use?                          */
} me_stop_order_t;

/* ─── Matching engine ────────────────────────────────────────────────────
 *
 * Owns the order book and the event queue.
 * All state lives here — no globals.
 *
 * NOTE: ob_book_t is ~255 KB (embedded pools). me_engine_t contains one,
 * so heap-allocate me_engine_t rather than placing it on the stack:
 *   me_engine_t *me = malloc(sizeof(me_engine_t));
 */
typedef struct {
    ob_book_t         book;               /* the limit order book             */
    me_event_queue_t  eq;                 /* outbound event ring              */
    uint64_t          trade_seq;          /* monotonic trade counter          */
    uint64_t          total_trades;
    uint64_t          total_qty_traded;
    uint64_t          orders_rejected;
    uint64_t          orders_accepted;
    me_top_of_book_t  last_tob;           /* previous TOB snapshot            */

    /* Registered event handler */
    me_handler_fn     handler;
    void             *handler_ctx;

    /* Stop order pool (dormant orders, not in the book) */
    me_stop_order_t   stops[ME_MAX_STOPS];
    uint32_t          stop_count;
    ob_price_t        last_trade_price;   /* updated after every fill          */

    /* match_scratch is stack-allocated inside me_submit_order() per call
     * to avoid embedding ~256KB of ob_match_t[] permanently in this struct. */
} me_engine_t;

/* ─── Submission result ──────────────────────────────────────────────────
 *
 * Returned synchronously to the caller of me_submit_order().
 */
typedef struct {
    ob_order_id_t id;           /* assigned order id (0 if rejected)         */
    ob_status_t   status;       /* OB_STATUS_OK / FILLED / PARTIAL / REJECTED */
    uint32_t      fills;        /* number of fills executed                  */
    ob_qty_t      qty_filled;   /* total qty filled across all fills         */
    ob_qty_t      qty_remain;   /* qty still resting (0 if fully filled)     */
    ob_price_t    avg_price;    /* volume-weighted average fill price         */
} me_result_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Lifecycle ---------------------------------------------------------------- */

/**
 * me_init – initialise the matching engine for `symbol`.
 * @param handler    optional event callback (may be NULL)
 * @param ctx        opaque pointer passed back to handler
 */
void me_init(me_engine_t   *me,
             const char    *symbol,
             me_handler_fn  handler,
             void          *ctx);

/**
 * me_destroy – release all resources owned by the engine.
 */
void me_destroy(me_engine_t *me);

/* Order entry ------------------------------------------------------------- */

/**
 * me_submit_order – primary entry point for all order types.
 *
 * @param me         the engine
 * @param type       LIMIT / MARKET / IOC / FOK
 * @param side       OB_SIDE_BID or OB_SIDE_ASK
 * @param price      limit price (ignored / set to 0 for MARKET orders)
 * @param qty        quantity (must be > 0)
 * @param out        receives the result (fills, avg_price, etc.)
 *
 * Emits one or more events into me->eq:
 *   ORDER_ACCEPTED or ORDER_REJECTED
 *   TRADE            (one per fill)
 *   ORDER_FILLED / ORDER_PARTIAL
 *   BOOK_UPDATED     (if top-of-book changed)
 */
void me_submit_order(me_engine_t     *me,
                     me_order_type_t  type,
                     ob_side_t        side,
                     ob_price_t       price,
                     ob_qty_t         qty,
                     me_result_t     *out);

/**
 * me_cancel_order – cancel a resting limit order by id.
 *
 * Emits ME_EVENT_ORDER_CANCELLED on success.
 * Returns false if the order is not found.
 */
bool me_cancel_order(me_engine_t *me, ob_order_id_t id);

/**
 * me_modify_order – atomically change price and/or qty.
 *
 * Emits ME_EVENT_ORDER_MODIFIED on success.
 * Returns false if not found or params invalid.
 */
bool me_modify_order(me_engine_t  *me,
                     ob_order_id_t id,
                     ob_price_t    new_price,
                     ob_qty_t      new_qty);

/* Event loop -------------------------------------------------------------- */

/**
 * me_run_events – drain the event queue, calling me->handler for each event.
 *
 * @return number of events dispatched.
 *
 * Call this after me_submit_order() (or in a dedicated consumer thread
 * once you add the lock-free ring).
 */
uint32_t me_run_events(me_engine_t *me);

/* Queries ------------------------------------------------------------------ */

/**
 * me_top_of_book – snapshot of current best bid/ask.
 */
me_top_of_book_t me_top_of_book(const me_engine_t *me);

/**
 * me_is_crossed – true if an immediate match is possible.
 * Useful for pre-flight checks.
 */
bool me_is_crossed(const me_engine_t *me,
                   ob_side_t          side,
                   ob_price_t         price);

/* Display ------------------------------------------------------------------ */

/**
 * me_print_stats – print engine statistics to stdout.
 */
void me_print_stats(const me_engine_t *me);

/* Stop orders ---------------------------------------------------------------- */

/**
 * me_submit_stop – submit a stop or stop-limit order.
 *
 * The order is stored dormant in me->stops[] and does NOT enter the book.
 * When last_trade_price crosses trigger_price, the stop activates and
 * re-enters as a market order (STOP) or limit order (STOP_LIMIT).
 *
 * @param trigger_price  price that activates the stop
 * @param limit_price    0 for stop-market, >0 for stop-limit
 * @param ttl_ns         time-to-live in nanoseconds (0 = GTC)
 */
void me_submit_stop(me_engine_t     *me,
                    me_order_type_t  type,
                    ob_side_t        side,
                    ob_price_t       trigger_price,
                    ob_price_t       limit_price,
                    ob_qty_t         qty,
                    uint64_t         ttl_ns,
                    me_result_t     *out);

/**
 * me_cancel_stop – cancel a dormant stop order by id.
 */
bool me_cancel_stop(me_engine_t *me, ob_order_id_t id);

/**
 * me_expire_orders – sweep expired day orders from the book and stop pool.
 *
 * @param now  current timestamp in nanoseconds
 * @return number of orders expired
 */
uint32_t me_expire_orders(me_engine_t *me, ob_ts_t now);

/* ─── Event type names (for logging) ─────────────────────────────────────── */

static inline const char *me_event_name(me_event_type_t t)
{
    switch (t) {
    case ME_EVENT_ORDER_ACCEPTED:  return "ACCEPTED";
    case ME_EVENT_ORDER_REJECTED:  return "REJECTED";
    case ME_EVENT_ORDER_FILLED:    return "FILLED";
    case ME_EVENT_ORDER_PARTIAL:   return "PARTIAL";
    case ME_EVENT_ORDER_CANCELLED: return "CANCELLED";
    case ME_EVENT_ORDER_MODIFIED:  return "MODIFIED";
    case ME_EVENT_TRADE:           return "TRADE";
    case ME_EVENT_BOOK_UPDATED:    return "BOOK_UPD";
    case ME_EVENT_STOP_TRIGGERED:  return "STOP_TRIG";
    default:                       return "UNKNOWN";
    }
}

static inline const char *me_order_type_name(me_order_type_t t)
{
    switch (t) {
    case ME_ORDER_LIMIT:      return "LIMIT";
    case ME_ORDER_MARKET:     return "MARKET";
    case ME_ORDER_IOC:        return "IOC";
    case ME_ORDER_FOK:        return "FOK";
    case ME_ORDER_STOP:       return "STOP";
    case ME_ORDER_STOP_LIMIT: return "STOP_LMT";
    default:                  return "?";
    }
}

#endif /* MATCHING_ENGINE_H */