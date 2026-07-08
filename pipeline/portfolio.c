/*
 * portfolio.c — implementation
 *
 * All math uses int64. Prices are already fixed-point (see orderbook.h).
 * Notional = price * qty.
 */

#include "portfolio.h"
#include <string.h>
#include <stdlib.h>

/* abs64 — branchless absolute value for signed 64-bit integers. */
static inline int64_t abs64(int64_t x) { return x < 0 ? -x : x; }

/* portfolio_init — zero everything and store the account's risk limits. */
void portfolio_init(portfolio_t *p,
                    uint32_t     account_id,
                    int64_t      starting_cash,
                    int64_t      max_position,
                    int64_t      max_order_notional)
{
    memset(p, 0, sizeof(*p));
    p->account_id         = account_id;
    p->cash               = starting_cash;
    p->max_position       = max_position;
    p->max_order_notional = max_order_notional;
}

/*
 * portfolio_on_fill — update cash, net position, average price, realised P&L.
 *
 * Two cases:
 *   (a) trade extends the position (same direction): VWAP-update avg_price.
 *   (b) trade reduces / flips the position: realise P&L on the closed slice.
 */
void portfolio_on_fill(portfolio_t *p,
                       ob_side_t    side,
                       ob_price_t   price,
                       ob_qty_t     qty)
{
    int64_t signed_qty = (side == OB_SIDE_BID) ? (int64_t)qty : -(int64_t)qty;
    int64_t notional   = (int64_t)price * (int64_t)qty;

    /* Cash: bid spends, ask receives. */
    p->cash += (side == OB_SIDE_BID) ? -notional : notional;

    int64_t old_qty = p->net_qty;
    int64_t new_qty = old_qty + signed_qty;

    if (old_qty == 0 || (old_qty > 0) == (signed_qty > 0)) {
        /* Same direction (or opening): update VWAP. */
        int64_t old_notional = (int64_t)p->avg_price * abs64(old_qty);
        int64_t add_notional = (int64_t)price        * (int64_t)qty;
        int64_t total_qty    = abs64(new_qty);
        p->avg_price = total_qty
                       ? (ob_price_t)((old_notional + add_notional) / total_qty)
                       : 0;
    } else {
        /* Closing direction: realise P&L on the closed portion. */
        int64_t close_qty = (abs64(signed_qty) < abs64(old_qty))
                            ? abs64(signed_qty) : abs64(old_qty);
        int64_t pnl_per   = (old_qty > 0)
                            ? ((int64_t)price - (int64_t)p->avg_price)
                            : ((int64_t)p->avg_price - (int64_t)price);
        p->realised_pnl  += pnl_per * close_qty;

        if (abs64(signed_qty) > abs64(old_qty)) {
            /* Position flipped: leftover opens the opposite side at trade px. */
            p->avg_price = price;
        }
        /* else: position shrank or closed; avg_price unchanged (or 0). */
        if (new_qty == 0) p->avg_price = 0;
    }

    p->net_qty = new_qty;
}

/*
 * portfolio_reserve — soft-check + reserve before an order enters the book.
 *
 * Returns false on breach. The pipeline's risk layer calls this and rejects
 * the order if it returns false.
 */
bool portfolio_reserve(portfolio_t *p,
                       ob_side_t    side,
                       ob_price_t   price,
                       ob_qty_t     qty)
{
    int64_t notional = (int64_t)price * (int64_t)qty;

    if (p->max_order_notional > 0 && notional > p->max_order_notional)
        return false;

    /* Pessimistic position check: assume the entire order fills. */
    int64_t projected = (side == OB_SIDE_BID)
                        ? p->net_qty + (int64_t)qty
                        : p->net_qty - (int64_t)qty;
    if (p->max_position > 0 && abs64(projected) > p->max_position)
        return false;

    if (side == OB_SIDE_BID) p->open_buy_notional += notional;
    else                     p->open_sell_qty     += (int64_t)qty;
    return true;
}

/* Mirror of portfolio_reserve — call on cancel / reject after admit. */
void portfolio_release(portfolio_t *p,
                       ob_side_t    side,
                       ob_price_t   price,
                       ob_qty_t     qty)
{
    int64_t notional = (int64_t)price * (int64_t)qty;
    if (side == OB_SIDE_BID) {
        p->open_buy_notional -= notional;
        if (p->open_buy_notional < 0) p->open_buy_notional = 0;
    } else {
        p->open_sell_qty -= (int64_t)qty;
        if (p->open_sell_qty < 0) p->open_sell_qty = 0;
    }
}

/* portfolio_unrealised_pnl — mark-to-market against a reference price. */
int64_t portfolio_unrealised_pnl(const portfolio_t *p, ob_price_t mark)
{
    if (p->net_qty == 0 || mark == 0) return 0;
    return (int64_t)(mark - p->avg_price) * p->net_qty;
}
