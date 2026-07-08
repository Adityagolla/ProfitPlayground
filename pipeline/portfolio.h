#ifndef PORTFOLIO_H
#define PORTFOLIO_H

/*
 * portfolio.h — Per-account positions, cash, realised + unrealised P&L
 *
 * Owns:
 *   - cash balance
 *   - net position (signed qty: +long, −short)
 *   - average entry price
 *   - realised P&L (closed trades)
 *   - exposure used for risk checks (notional locked behind open orders)
 *
 * Single-symbol for clarity; in production this would be a hash by symbol.
 */

#include "orderbook.h"   /* ob_price_t, ob_qty_t, ob_side_t                  */
#include <stdint.h>

typedef struct {
    uint32_t   account_id;

    /* Cash and position */
    int64_t    cash;             /* fixed-point currency units               */
    int64_t    net_qty;          /* +long / −short                            */
    ob_price_t avg_price;        /* VWAP of current open position             */

    /* P&L */
    int64_t    realised_pnl;

    /* Risk reservations: notional locked by working (resting) orders        */
    int64_t    open_buy_notional;
    int64_t    open_sell_qty;

    /* Limits — used by the risk layer */
    int64_t    max_position;     /* abs(net_qty) limit                       */
    int64_t    max_order_notional;
} portfolio_t;

/* Initialise an account with starting cash and limits. */
void portfolio_init(portfolio_t *p,
                    uint32_t     account_id,
                    int64_t      starting_cash,
                    int64_t      max_position,
                    int64_t      max_order_notional);

/* Apply a fill at (price, qty) on `side`. Updates cash, net_qty, P&L. */
void portfolio_on_fill(portfolio_t *p,
                       ob_side_t    side,
                       ob_price_t   price,
                       ob_qty_t     qty);

/* Reserve risk capacity when a NEW order is admitted.
 * Returns false if it would breach limits — caller must reject. */
bool portfolio_reserve(portfolio_t *p,
                       ob_side_t    side,
                       ob_price_t   price,
                       ob_qty_t     qty);

/* Release a reservation when an order is cancelled or rejected post-admit. */
void portfolio_release(portfolio_t *p,
                       ob_side_t    side,
                       ob_price_t   price,
                       ob_qty_t     qty);

/* Mark-to-market unrealised P&L given a reference price (e.g. last trade). */
int64_t portfolio_unrealised_pnl(const portfolio_t *p, ob_price_t mark);

#endif /* PORTFOLIO_H */
