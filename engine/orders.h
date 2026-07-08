#ifndef ORDERS_H
#define ORDERS_H

/*
 * orders.h — Order validation, pre-processing, and per-type flow logic
 *
 * What this layer owns
 * ────────────────────
 *  1. Structural validation     — qty > 0, valid side
 *  2. Type-specific validation  — limit needs price > 0, market needs price == 0
 *  3. FOK liquidity pre-check   — read-only walk of opposite side
 *  4. Cross-check               — does this order cross the book right now?
 *  5. Effective price           — market orders get a sentinel so the book's
 *                                 matching loop always sees a crossable price
 *  6. Flow routing              — returns order_flow_t so matching_engine.c
 *                                 calls the right path with zero branching on type
 *
 * What this layer does NOT own
 * ────────────────────────────
 *  - No book mutations          (matching_engine.c calls ob_add_order etc.)
 *  - No event emission          (matching_engine.c owns the event queue)
 *  - No memory allocation       (all params, no heap)
 *
 * How matching_engine.c uses this
 * ─────────────────────────────────
 *
 *   order_request_t req;
 *   orders_preprocess(&me->book, type, side, price, qty, &req);
 *
 *   if (req.flow == FLOW_REJECT) { emit_rejected(..., req.err_msg); return; }
 *
 *   // submit req.eff_price (not the raw price) to ob_add_order
 *   ob_add_order(&me->book, side, req.eff_price, qty, ...);
 *
 *   // IOC: cancel remainder after ob_add_order returns
 *   // FOK: guaranteed to fill (liquidity pre-checked)
 */

#include "matching_engine.h"   /* for me_order_type_t, me_engine_t types     */

/* ─── Flow decision returned to the engine ──────────────────────────────── */

typedef enum {
    FLOW_LIMIT,     /* cross check first; rest remainder if not crossed      */
    FLOW_MARKET,    /* always crosses; eff_price = sentinel; never rests     */
    FLOW_IOC,       /* same as limit cross; engine cancels remainder after   */
    FLOW_FOK,       /* liquidity pre-checked; engine submits, guaranteed fill*/
    FLOW_STOP,      /* dormant stop order; engine stores, does NOT submit    */
    FLOW_REJECT,    /* validation failed; engine emits REJECTED, stops       */
} order_flow_t;

/* ─── Pre-processed order request ──────────────────────────────────────── */

/*
 * order_request_t — fully resolved, validated order handed to the engine.
 *
 * After orders_preprocess() returns:
 *   flow      — which engine path to take
 *   eff_price — price to pass to ob_add_order (may differ from raw price
 *               for market orders which get a sentinel value)
 *   err_msg   — human-readable reason, set only when flow == FLOW_REJECT
 */
typedef struct {
    me_order_type_t  type;
    ob_side_t        side;
    ob_price_t       price;        /* original price as submitted            */
    ob_price_t       eff_price;    /* effective price for ob_add_order       */
    ob_qty_t         qty;
    order_flow_t     flow;
    const char      *err_msg;      /* points to a string literal, no alloc  */
    ob_price_t       trigger_price; /* stop activation price (0 = not stop) */
    ob_ts_t          expiry_ts;     /* auto-cancel time (0 = GTC)           */
} order_request_t;

/* ─── Sentinel prices for market orders ─────────────────────────────────── */

/*
 * A market BID must cross any resting ASK → use INT64_MAX so
 * (bid_price >= best_ask) is always true.
 *
 * A market ASK must cross any resting BID → use 1 so
 * (ask_price <= best_bid) is always true (all valid bids are > 0).
 */
#define MARKET_BID_PRICE  ((ob_price_t)0x7FFFFFFFFFFFFFFFLL)
#define MARKET_ASK_PRICE  ((ob_price_t)1)

static inline ob_price_t orders_market_sentinel(ob_side_t side)
{
    return (side == OB_SIDE_BID) ? MARKET_BID_PRICE : MARKET_ASK_PRICE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API — called only from matching_engine.c
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * orders_preprocess — validate + pre-process one raw order submission.
 *
 * Reads the book read-only (only for FOK liquidity check and cross-check).
 * Never writes to the book, never emits events, never allocates memory.
 *
 * On return:
 *   out->flow == FLOW_REJECT  →  out->err_msg explains why; do not submit.
 *   out->flow != FLOW_REJECT  →  submit out->eff_price to ob_add_order.
 */
void orders_preprocess(const ob_book_t *book,
                       me_order_type_t  type,
                       ob_side_t        side,
                       ob_price_t       price,
                       ob_qty_t         qty,
                       order_request_t *out);

/*
 * orders_crosses — true if submitting (side, price, type) would immediately
 *                  match against the current book.
 *
 * Market orders always return true.
 * Exposed so the engine can branch on cross before calling ob_add_order,
 * and so callers can do pre-trade cross checks without submitting.
 */
bool orders_crosses(const ob_book_t *book,
                    ob_side_t        side,
                    ob_price_t       price,
                    me_order_type_t  type);

/*
 * orders_fok_check — true if the book holds enough resting qty to fill
 *                    `qty` on `side` at prices at least as good as `price`.
 *
 * Read-only walk of the opposite side's level array.
 * Exposed separately so callers can run pre-trade FOK checks.
 */
bool orders_fok_check(const ob_book_t *book,
                      ob_side_t        side,
                      ob_price_t       price,
                      ob_qty_t         qty);

#endif /* ORDERS_H */