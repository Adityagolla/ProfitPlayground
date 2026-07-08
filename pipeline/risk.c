/*
 * risk.c — Pre-trade risk implementation
 *
 * Runs after schema validation and before the sequencer. Stateless except
 * for last_trade_price, which the pipeline updates from the event stream.
 */

#include "risk.h"
#include <string.h>
#include <stdlib.h>

/* abs64 — helper for signed price-deviation checks. */
static inline int64_t abs64(int64_t x) { return x < 0 ? -x : x; }

/* risk_init — fill in conservative defaults. */
void risk_init(risk_engine_t *r)
{
    memset(r, 0, sizeof(*r));
    r->tick_size       = 1;
    r->fat_finger_bps  = 1000;   /* 10% band */
}

/* risk_on_trade — pipeline calls this on every TRADE event. */
void risk_on_trade(risk_engine_t *r, ob_price_t price)
{
    r->last_trade_price = price;
}

/*
 * risk_check — main entry. Returns false on reject and writes a reason.
 *
 * Cancels and modifies are passed through (no margin re-check here for
 * simplicity; in production a modify increasing qty would re-reserve).
 */
bool risk_check(risk_engine_t       *r,
                portfolio_t         *p,
                const order_event_t *ev,
                const char         **reason)
{
    if (ev->action != GW_ACT_NEW) {
        r->accepted++;
        return true;
    }

    /* 1. Tick size — limit-priced orders must be on grid. */
    if (r->tick_size > 1 && ev->price > 0 && (ev->price % r->tick_size) != 0) {
        *reason = "price not on tick grid";
        r->rejected++;
        return false;
    }

    /* 2. Fat-finger — only meaningful once we have a reference price. */
    if (r->last_trade_price > 0 && r->fat_finger_bps > 0 && ev->price > 0
        && (ev->type == ME_ORDER_LIMIT || ev->type == ME_ORDER_IOC ||
            ev->type == ME_ORDER_FOK   || ev->type == ME_ORDER_STOP_LIMIT)) {
        int64_t band = (int64_t)r->last_trade_price
                     * (int64_t)r->fat_finger_bps / 10000;
        int64_t dev  = abs64((int64_t)ev->price - (int64_t)r->last_trade_price);
        if (dev > band) {
            *reason = "fat-finger: price too far from last";
            r->rejected++;
            return false;
        }
    }

    /* 3. Portfolio reservation — margin / position limits. */
    ob_price_t reserve_px = (ev->price > 0) ? ev->price : r->last_trade_price;
    if (reserve_px == 0) reserve_px = 1;   /* unknown market price → tiny */
    if (!portfolio_reserve(p, ev->side, reserve_px, ev->qty)) {
        *reason = "risk: position or notional limit breached";
        r->rejected++;
        return false;
    }

    r->accepted++;
    return true;
}
