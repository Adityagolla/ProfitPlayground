#ifndef RISK_H
#define RISK_H

/*
 * risk.h — Pre-trade risk layer
 *
 * Sits between Validation and the Sequencer. Reads portfolio state and
 * decides whether the order can pass. Performs:
 *   - tick-size & price sanity
 *   - fat-finger guard (price within ±X% of last)
 *   - portfolio reservation (margin / position limits)
 *
 * Independent of the matching engine — never touches the book.
 */

#include "gateway.h"
#include "portfolio.h"
#include <stdbool.h>

typedef struct {
    /* Reference last trade price (mirror of ME's last_trade_price).
     * Updated by the pipeline whenever a TRADE event is published. */
    ob_price_t  last_trade_price;

    /* Fat-finger band: reject limit orders priced more than this percent
     * away from last_trade_price. 0 = disabled. */
    uint32_t    fat_finger_bps;     /* basis points; e.g. 500 = 5%          */

    /* Minimum price increment. 0 = disabled. */
    ob_price_t  tick_size;

    /* Counters */
    uint64_t    rejected;
    uint64_t    accepted;
} risk_engine_t;

/* Initialise with defaults: tick_size=1, fat_finger_bps=1000 (10%). */
void risk_init(risk_engine_t *r);

/*
 * risk_check — run all pre-trade checks against `p` for `ev`.
 *
 * On accept, also calls portfolio_reserve() so the order is accounted for.
 * On reject, sets *reason and returns false.
 */
bool risk_check(risk_engine_t      *r,
                portfolio_t        *p,
                const order_event_t *ev,
                const char        **reason);

/* Notify risk that a trade happened so it can update last_trade_price. */
void risk_on_trade(risk_engine_t *r, ob_price_t price);

#endif /* RISK_H */
