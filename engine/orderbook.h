#ifndef ORDER_BOOK_H
#define ORDER_BOOK_H

/*
 * orderbook.h — Limit order book with preallocated pools
 *
 * Phase 1 (original): correctness-first, pointer-based, malloc on hot path.
 * Phase 2 (current):  preallocated memory pools, cache-friendly index-based
 *                     structs, optional multi-threading via compile flag.
 *
 * The book itself performs Price-Time priority matching when a new order
 * crosses the opposite side. Fill records are written into a caller-supplied
 * scratch buffer (ob_match_t array) so the matching engine can emit events.
 * Matching / crossing logic is handled externally.
 *
 * Terminology
 * -----------
 *  Order  – a single resting limit instruction (buy or sell at a price)
 *  Level  – all orders at the same price, kept as a FIFO queue
 *  Side   – the bids (buy) side or the asks (sell) side
 *  Book   – one bid side + one ask side for a single instrument
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ─── Sizing constants ──────────────────────────────────────────────────── */

#define OB_MAX_SYMBOL_LEN   16   /* instrument ticker, e.g. "AAPL\0"        */
#define OB_MAX_LEVELS       256  /* max distinct price levels per side       */
#define OB_MAX_ORDERS       4096 /* max live orders in the whole book        */

/* ─── Core identifiers ──────────────────────────────────────────────────── */

typedef uint64_t ob_order_id_t;   /* unique order handle returned to caller  */
typedef int64_t  ob_price_t;      /* price in fixed-point (e.g. cents × 100) */
typedef uint64_t ob_qty_t;        /* quantity in base units                  */
typedef uint64_t ob_ts_t;         /* timestamp in nanoseconds since epoch    */

/* ─── Enumerations ──────────────────────────────────────────────────────── */

typedef enum {
    OB_SIDE_BID = 0,   /* buyer: wants to BUY,  price descending            */
    OB_SIDE_ASK = 1,   /* seller: wants to SELL, price ascending            */
    OB_SIDE_COUNT
} ob_side_t;

typedef enum {
    OB_STATUS_OK             =  0,   /* order resting, zero fills            */
    OB_STATUS_REJECTED       = -1,   /* invalid params / pool at capacity    */
    OB_STATUS_NOT_FOUND      = -2,   /* cancel/modify on unknown order id    */
    OB_STATUS_ALREADY_FILLED = -3,
    OB_STATUS_FILLED         =  1,   /* aggressor fully filled               */
    OB_STATUS_PARTIAL        =  2,   /* aggressor partially filled, resting  */
} ob_status_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * ▓▓▓ PHASE 2 BEGINS ▓▓▓ — Preallocated pools · Cache-friendly layout
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Sentinel index: "no node" for all pool index fields.                      */
#define OB_NULL_IDX  UINT16_MAX

/* ─── Order structure (cache-friendly, 64 bytes = 1 cache line) ──────────── */

/*
 * ob_order — represents one resting limit order.
 *
 * Layout optimised so hot fields (id, price, qty_remain, side) sit in the
 * first 32 bytes.  prev_idx / next_idx are pool indices into
 * book->order_pool[], replacing 8-byte pointers with 2-byte indices.
 * This keeps the FIFO queue contiguous and cache-prefetcher friendly.
 */
typedef struct ob_order {
    /* ── Hot fields (first cache line) ──────────────────────────────────── */
    ob_order_id_t  id;          /* unique id assigned at submission          */
    ob_price_t     price;       /* limit price (fixed-point)                 */
    ob_qty_t       qty_remain;  /* unfilled quantity (decremented on match)  */
    ob_side_t      side;        /* bid or ask                                */
    uint16_t       prev_idx;    /* FIFO queue: previous order (pool index)   */
    uint16_t       next_idx;    /* FIFO queue: next order     (pool index)   */

    /* ── Cold fields ────────────────────────────────────────────────────── */
    ob_qty_t       qty;         /* original quantity                         */
    ob_ts_t        ts_submit;   /* submission timestamp (ns)                 */
    ob_ts_t        ts_update;   /* last modification timestamp (ns)          */
    ob_ts_t        expiry_ts;   /* auto-cancel time (0 = GTC / no expiry)    */
} ob_order_t;

/* ─── Price level (cache-friendly, ~40 bytes) ───────────────────────────── */

/*
 * ob_level — all orders resting at one price point.
 *
 * Maintains a FIFO queue via pool indices (head_idx = oldest/highest-priority).
 * AVL tree linkage also uses pool indices into book->level_pool[].
 */
typedef struct ob_level {
    ob_price_t       price;
    ob_qty_t         total_qty;    /* sum of qty_remain across all orders    */
    uint32_t         order_count;

    /* FIFO queue endpoints (order pool indices) */
    uint16_t         head_idx;     /* oldest order — matched first (FIFO)    */
    uint16_t         tail_idx;     /* newest order — appended here           */

    /* AVL tree linkage (level pool indices) */
    uint16_t         left_idx;
    uint16_t         right_idx;
    int              height;       /* AVL balance field                      */
} ob_level_t;

/* ─── One side of the book (bids OR asks) ───────────────────────────────── */

/*
 * ob_side_s — owns the price-level AVL tree root and aggregate stats.
 *
 * The AVL tree gives O(log n) level lookup, insertion, and deletion.
 * root_idx is a level pool index (OB_NULL_IDX when empty).
 */
typedef struct {
    ob_side_t   side;
    uint16_t    root_idx;       /* AVL tree root (level pool index)          */
    uint32_t    level_count;    /* distinct price levels currently live      */
    uint64_t    total_qty;      /* sum of all resting qty on this side       */
} ob_side_t_s;

/* ─── Threading (opt-in: compile with -DOB_ENABLE_THREADS) ──────────────── */

#ifdef OB_ENABLE_THREADS
#include <stdatomic.h>

/*
 * Lightweight read-write spinlock.
 *   state ==  0 → unlocked
 *   state == -1 → write-locked
 *   state  >  0 → reader count
 */
typedef struct {
    _Atomic int32_t state;
} ob_rwlock_t;
#endif

/* ─── The order book ────────────────────────────────────────────────────── */

typedef struct {
    char          symbol[OB_MAX_SYMBOL_LEN];
    ob_side_t_s   sides[OB_SIDE_COUNT];   /* [0]=bids, [1]=asks             */
    ob_order_id_t next_order_id;          /* monotonically increasing       */

    /* Statistics */
    uint64_t      total_orders_added;
    uint64_t      total_orders_cancelled;

    /* ── Phase 2: preallocated order pool (zero malloc on hot path) ────── */
    ob_order_t    order_pool[OB_MAX_ORDERS];
    uint16_t      order_free_stack[OB_MAX_ORDERS];
    uint16_t      order_free_top;          /* free-stack pointer            */

    /* ── Phase 2: preallocated level pool ─────────────────────────────── */
    ob_level_t    level_pool[OB_MAX_LEVELS * OB_SIDE_COUNT];
    uint16_t      level_free_stack[OB_MAX_LEVELS * OB_SIDE_COUNT];
    uint16_t      level_free_top;

    /* ── Phase 2: per-book lookup table (order id → pool index) ────────── */
    /* Embedded directly for cache locality instead of a global pointer.   */

    /* ── Phase 2: threading ────────────────────────────────────────────── */
#ifdef OB_ENABLE_THREADS
    ob_rwlock_t   lock;
#endif
} ob_book_t;

/* ─── Fill record — one entry per matched resting order ─────────────────────
 *
 * ob_match_t is written into a caller-supplied scratch buffer by ob_add_order
 * when the incoming order crosses the opposite side.  The matching engine
 * reads these to emit TRADE events without needing to touch the book again.
 */
typedef struct {
    ob_order_id_t resting_id;  /* id of the resting order that was filled    */
    ob_price_t    price;       /* execution price (resting order's price)     */
    ob_qty_t      qty;         /* quantity filled in this match               */
    ob_ts_t       ts;          /* timestamp of the fill (nanoseconds)        */
} ob_match_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * ▓▓▓ PHASE 2 ENDS ▓▓▓
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ─── Public API ────────────────────────────────────────────────────────── */

/*
 * Lifecycle
 * ---------
 */

/**
 * ob_init – initialise a book for `symbol`.
 * Populates free stacks for both pools. Must be called before any other
 * function on this book.
 *
 * NOTE: ob_book_t is ~400 KB due to embedded pools.  Heap-allocate it
 *       (malloc) rather than placing on the stack.
 */
void ob_init(ob_book_t *book, const char *symbol);

/**
 * ob_destroy – reset the book.
 * No heap memory to free (pools are embedded).  After this call the
 * ob_book_t may be re-initialised or discarded.
 */
void ob_destroy(ob_book_t *book);

/*
 * Order management
 * ----------------
 */

/**
 * STORE / MATCH ORDER — validate, match against opposite side, then rest
 * any unfilled remainder.
 *
 * @param book          the target book
 * @param side          OB_SIDE_BID or OB_SIDE_ASK
 * @param price         limit price (fixed-point, must be > 0)
 * @param qty           quantity (must be > 0)
 * @param out_id        receives the assigned order id (0 on REJECTED)
 * @param match_scratch caller-supplied array to receive fill records
 *                      (may be NULL if the caller does not need fills)
 * @param match_count   out: number of fills written into match_scratch
 *
 * Matching follows Price-Time priority:
 *   1. Walk the opposite side's AVL tree from best price.
 *   2. For each level: consume FIFO queue head-first until either the
 *      aggressor qty_remain == 0 or the level is exhausted.
 *   3. Stop when no more crossing levels exist.
 *   4. Rest any unfilled remainder in the aggressor's own side.
 *
 * @return OB_STATUS_OK       – resting, zero fills
 *         OB_STATUS_FILLED   – aggressor fully filled
 *         OB_STATUS_PARTIAL  – partially filled, remainder resting
 *         OB_STATUS_REJECTED – invalid params or pool at capacity
 */
ob_status_t ob_add_order(ob_book_t     *book,
                         ob_side_t      side,
                         ob_price_t     price,
                         ob_qty_t       qty,
                         ob_ts_t        expiry_ts,
                         ob_order_id_t *out_id,
                         ob_match_t    *match_scratch,
                         uint32_t      *match_count);

/**
 * REMOVE ORDER — detach from level queue, return slot to pool.
 *
 * @return OB_STATUS_OK        – cancelled successfully
 *         OB_STATUS_NOT_FOUND – id unknown or already filled
 */
ob_status_t ob_cancel_order(ob_book_t *book, ob_order_id_t id);

/**
 * MODIFY ORDER — remove from old level, update fields, re-insert.
 *
 * Changing price loses time priority (order moves to tail of new level).
 * Reducing qty within the same price keeps time priority.
 *
 * @return OB_STATUS_OK        – modified
 *         OB_STATUS_NOT_FOUND – id unknown or already filled
 *         OB_STATUS_REJECTED  – new_price or new_qty invalid
 */
ob_status_t ob_modify_order(ob_book_t    *book,
                            ob_order_id_t id,
                            ob_price_t    new_price,
                            ob_qty_t      new_qty);

/*
 * Queries
 * -------
 */

/**
 * ob_best_bid – best (highest) resting bid price, or 0 if empty.
 */
ob_price_t ob_best_bid(const ob_book_t *book);

/**
 * ob_best_ask – best (lowest) resting ask price, or 0 if empty.
 */
ob_price_t ob_best_ask(const ob_book_t *book);

/**
 * ob_spread – ask - bid, or -1 if either side is empty.
 */
ob_price_t ob_spread(const ob_book_t *book);

/**
 * ob_mid_price – (bid + ask) / 2 in fixed-point, or 0 if either empty.
 */
ob_price_t ob_mid_price(const ob_book_t *book);

/**
 * ob_depth – total resting qty on the given side within `price_range`
 *            ticks of the best price.  Pass UINT64_MAX to sum all levels.
 */
ob_qty_t ob_depth(const ob_book_t *book,
                  ob_side_t        side,
                  ob_price_t       price_range);

/**
 * ob_top_levels – copy up to `depth` best price levels for `side` into
 * caller-supplied arrays, best price first (ascending for asks, descending
 * for bids) — i.e. index 0 is always the top of book for that side.
 *
 * @return number of levels written (<= depth).
 */
uint32_t ob_top_levels(const ob_book_t *book,
                       ob_side_t        side,
                       uint32_t         depth,
                       ob_price_t      *prices_out,
                       ob_qty_t        *qtys_out);

/**
 * ob_get_order – retrieve a snapshot of a resting order by id.
 *
 * @return true if found and `out` filled, false otherwise.
 */
bool ob_get_order(const ob_book_t *book,
                  ob_order_id_t    id,
                  ob_order_t      *out);

/*
 * Display / debug
 * ---------------
 */

/**
 * ob_print – pretty-print the book state to stdout.
 * @param levels  max price levels to show per side (0 = all)
 */
void ob_print(const ob_book_t *book, uint32_t levels);

/**
 * ob_print_stats – print lifetime statistics to stdout.
 */
void ob_print_stats(const ob_book_t *book);

/* ─── Utility macros ────────────────────────────────────────────────────── */

/** Convert a human price (double) to fixed-point (price × 10000). */
#define OB_PRICE_FROM_DOUBLE(d)   ((ob_price_t)((d) * 10000.0 + 0.5))

/** Convert fixed-point back to double for display. */
#define OB_PRICE_TO_DOUBLE(p)     ((double)(p) / 10000.0)

#endif /* ORDER_BOOK_H */