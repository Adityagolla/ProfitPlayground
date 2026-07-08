/*
 * orders.c — Order validation, pre-processing, and per-type flow logic
 */

#include "orders.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Section 1 — Structural validation
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *validate_structural(ob_side_t side, ob_qty_t qty)
{
    if (qty == 0)              return "qty must be > 0";
    if (side >= OB_SIDE_COUNT) return "invalid side";
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Section 2 — Type-specific validation
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *validate_limit(ob_price_t price)
{
    if (price <= 0) return "limit price must be > 0";
    return NULL;
}

static const char *validate_market(ob_price_t price)
{
    if (price != 0) return "market order must have price = 0";
    return NULL;
}

static const char *validate_fok(const ob_book_t *book,
                                ob_side_t        side,
                                ob_price_t       price,
                                ob_qty_t         qty)
{
    const char *err = validate_limit(price);
    if (err) return err;

    if (!orders_fok_check(book, side, price, qty))
        return "FOK: insufficient liquidity";

    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Section 3 — Cross-check
 *
 *   BID crosses when  bid_price >= best_ask  (willing to pay at least the ask)
 *   ASK crosses when  ask_price <= best_bid  (willing to sell at most the bid)
 *   MARKET crosses only when the opposite side has liquidity — returning true
 *   on an empty book would mislead callers into thinking a fill will happen.
 * ═══════════════════════════════════════════════════════════════════════════ */

bool orders_crosses(const ob_book_t *book,
                    ob_side_t        side,
                    ob_price_t       price,
                    me_order_type_t  type)
{
    if (type == ME_ORDER_MARKET) {
        /* Market only crosses if there is actually liquidity to hit */
        ob_price_t best = (side == OB_SIDE_BID) ? ob_best_ask(book)
                                                 : ob_best_bid(book);
        return best > 0;
    }

    if (side == OB_SIDE_BID) {
        ob_price_t best_ask = ob_best_ask(book);
        return best_ask > 0 && price >= best_ask;
    } else {
        ob_price_t best_bid = ob_best_bid(book);
        return best_bid > 0 && price <= best_bid;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Section 4 — FOK liquidity check (pool-index AVL walk with subtree pruning)
 *
 * Walks the opposite side's AVL tree accumulating resting qty at prices
 * that would cross the aggressor's limit.  Early-exits once enough qty
 * is found OR when a subtree is guaranteed to contain only non-crossable
 * prices.
 *
 * Pruning strategy (the key optimisation):
 *
 *   fok_sum_asc (BID hitting ASKs, ascending walk):
 *     We want ask prices <= limit.  In a BST, all nodes in the right
 *     subtree have prices > this node.  So if THIS node's price already
 *     exceeds the limit, the entire right subtree is uncrossable — we
 *     recurse only into the left child (which may still hold cheaper asks).
 *     When the node IS crossable we visit left, count self, then right.
 *
 *   fok_sum_desc (ASK hitting BIDs, descending walk):
 *     Mirror image.  We want bid prices >= limit.  If this node's price
 *     is below the limit, the left subtree is entirely uncrossable.
 *
 * Worst case is still O(k) where k = number of crossable levels, but
 * the pruning avoids touching any subtree that sits entirely outside
 * the crossable range — often cutting the walk in half or better.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    ob_price_t limit;   /* aggressor's limit price                            */
    ob_qty_t   accum;   /* qty accumulated so far                             */
    ob_qty_t   need;    /* qty needed to satisfy the FOK                      */
} fok_walk_t;

/* Index → pointer into level pool */
#define FOK_LV(book, i)  (&(book)->level_pool[(i)])

/*
 * Ascending in-order walk (left → self → right) — BID aggressor hitting ASKs.
 * Counts qty from ask levels whose price <= aggressor's limit.
 * Prunes: if node price > limit, skip right subtree entirely.
 */
static void fok_sum_asc(const ob_book_t *book,
                        uint16_t         node_idx,
                        fok_walk_t      *w)
{
    if (node_idx == OB_NULL_IDX || w->accum >= w->need) return;

    const ob_level_t *lv = FOK_LV(book, node_idx);

    if (lv->price > w->limit) {
        /* This node and its right subtree are all > limit.
         * Only the left subtree can hold cheaper (crossable) asks. */
        fok_sum_asc(book, lv->left_idx, w);
        return;
    }

    /* Node is crossable — standard in-order: left, self, right */
    fok_sum_asc(book, lv->left_idx, w);
    w->accum += lv->total_qty;
    if (w->accum < w->need)
        fok_sum_asc(book, lv->right_idx, w);
}

/*
 * Descending reverse-in-order walk (right → self → left) — ASK aggressor
 * hitting BIDs.  Counts qty from bid levels whose price >= aggressor's limit.
 * Prunes: if node price < limit, skip left subtree entirely.
 */
static void fok_sum_desc(const ob_book_t *book,
                         uint16_t         node_idx,
                         fok_walk_t      *w)
{
    if (node_idx == OB_NULL_IDX || w->accum >= w->need) return;

    const ob_level_t *lv = FOK_LV(book, node_idx);

    if (lv->price < w->limit) {
        /* This node and its left subtree are all < limit.
         * Only the right subtree can hold more expensive (crossable) bids. */
        fok_sum_desc(book, lv->right_idx, w);
        return;
    }

    /* Node is crossable — reverse in-order: right, self, left */
    fok_sum_desc(book, lv->right_idx, w);
    w->accum += lv->total_qty;
    if (w->accum < w->need)
        fok_sum_desc(book, lv->left_idx, w);
}

/* Public entry: returns true if the book can fill the entire FOK qty */
bool orders_fok_check(const ob_book_t *book,
                      ob_side_t        side,
                      ob_price_t       price,
                      ob_qty_t         qty)
{
    fok_walk_t w = { .limit = price, .accum = 0, .need = qty };

    if (side == OB_SIDE_BID)
        fok_sum_asc(book, book->sides[OB_SIDE_ASK].root_idx, &w);
    else
        fok_sum_desc(book, book->sides[OB_SIDE_BID].root_idx, &w);

    return w.accum >= qty;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Section 5 — Pre-processing entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Rejection helper — sets FLOW_REJECT + err_msg if err is non-NULL.
 * Returns true (= caller should return) when rejection fires. */
static inline bool reject_if(order_request_t *out, const char *err)
{
    if (!err) return false;
    out->flow    = FLOW_REJECT;
    out->err_msg = err;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Section 6 — Stop order validation
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *validate_stop(ob_price_t trigger_price, ob_price_t limit_price)
{
    if (trigger_price <= 0) return "stop trigger price must be > 0";
    if (limit_price != 0)   return "stop-market must have limit_price = 0";
    return NULL;
}

static const char *validate_stop_limit(ob_price_t trigger_price, ob_price_t limit_price)
{
    if (trigger_price <= 0) return "stop trigger price must be > 0";
    if (limit_price <= 0)   return "stop-limit must have limit_price > 0";
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Section 7 — Pre-processing entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

void orders_preprocess(const ob_book_t *book,
                       me_order_type_t  type,
                       ob_side_t        side,
                       ob_price_t       price,
                       ob_qty_t         qty,
                       order_request_t *out)
{
    memset(out, 0, sizeof(*out));
    out->type  = type;
    out->side  = side;
    out->price = price;
    out->qty   = qty;

    if (reject_if(out, validate_structural(side, qty))) return;

    switch (type) {
    case ME_ORDER_LIMIT:
        if (reject_if(out, validate_limit(price))) return;
        out->eff_price = price;
        out->flow      = FLOW_LIMIT;
        break;

    case ME_ORDER_MARKET:
        if (reject_if(out, validate_market(price))) return;
        out->eff_price = orders_market_sentinel(side);
        out->flow      = FLOW_MARKET;
        break;

    case ME_ORDER_IOC:
        if (reject_if(out, validate_limit(price))) return;
        out->eff_price = price;
        out->flow      = FLOW_IOC;
        break;

    case ME_ORDER_FOK:
        if (reject_if(out, validate_fok(book, side, price, qty))) return;
        out->eff_price = price;
        out->flow      = FLOW_FOK;
        break;

    case ME_ORDER_STOP:
        if (reject_if(out, validate_stop(out->trigger_price, price))) return;
        out->flow = FLOW_STOP;
        break;

    case ME_ORDER_STOP_LIMIT:
        if (reject_if(out, validate_stop_limit(out->trigger_price, price))) return;
        out->eff_price = price;   /* limit price used when triggered */
        out->flow      = FLOW_STOP;
        break;

    default:
        out->flow    = FLOW_REJECT;
        out->err_msg = "unknown order type";
        break;
    }
}
