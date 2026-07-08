/*
 * matching_engine.c — Full matching engine implementation
 *
 * Enable proper C99 printf format specifiers on MinGW (fix #7)
 */
#if defined(__MINGW32__) || defined(__MINGW64__)
#define __USE_MINGW_ANSI_STDIO 1
#endif

/*
 * Flow for every submitted order
 * ─────────────────────────────────────────────────────────────────────────────
 *
 *  me_submit_order()
 *    │
 *    ├─ orders_preprocess()     ← rejects bad params, resolves effective price
 *    │
 *    ├─ ob_add_order()          ← does the actual Price-Time matching + resting
 *    │       returns fill records in a stack-local ob_match_t scratch buffer
 *    │       returns OB_STATUS_FILLED / PARTIAL / OK / REJECTED
 *    │
 *    ├─ [IOC] cancel any unfilled remainder that rested
 *    │
 *    ├─ emit_order_event()      ACCEPTED / PARTIAL / FILLED / REJECTED
 *    ├─ emit_trade()            one ME_EVENT_TRADE per fill
 *    │     └─ check_stops()     scan dormant stops against last trade price
 *    └─ maybe_emit_tob()        if best bid or ask changed
 */

#include "matching_engine.h"
#include "orders.h"
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * TIMESTAMP — cross-platform monotonic nanosecond clock (fix #6)
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#include <windows.h>
static ob_ts_t me_now(void)
{
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (ob_ts_t)(count.QuadPart * 1000000000ULL / freq.QuadPart);
}
#else
#include <time.h>
static ob_ts_t me_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ob_ts_t)ts.tv_sec * 1000000000ULL + (ob_ts_t)ts.tv_nsec;
}
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * EVENT QUEUE HELPERS — fixed-capacity ring buffer
 * ═══════════════════════════════════════════════════════════════════════════ */

#define EQ_MASK  (ME_EVENT_QUEUE_CAP - 1)

static bool eq_push(me_event_queue_t *eq, const me_event_t *ev)
{
    uint32_t next = (eq->head + 1) & EQ_MASK;
    if (next == eq->tail) { eq->dropped++; return false; }
    eq->events[eq->head] = *ev;
    eq->head = next;
    return true;
}

static bool eq_pop(me_event_queue_t *eq, me_event_t *out)
{
    if (eq->tail == eq->head) return false;
    *out = eq->events[eq->tail];
    eq->tail = (eq->tail + 1) & EQ_MASK;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * EVENT EMISSION HELPERS
 * ═══════════════════════════════════════════════════════════════════════════ */

static void emit_order_event(me_engine_t     *me,
                             me_event_type_t  type,
                             ob_order_id_t    id,
                             ob_side_t        side,
                             me_order_type_t  otype,
                             ob_price_t       price,
                             ob_qty_t         qty_orig,
                             ob_qty_t         qty_remain,
                             ob_qty_t         qty_filled,
                             const char      *reject_reason)
{
    me_event_t ev = {
        .type         = type,
        .ts           = me_now(),
        .order_id     = id,
        .side         = side,
        .order_type   = otype,
        .price        = price,
        .qty_original = qty_orig,
        .qty_remain   = qty_remain,
        .qty_filled   = qty_filled,
    };
    if (reject_reason)
        snprintf(ev.reject_reason, sizeof(ev.reject_reason),
                 "%s", reject_reason);
    eq_push(&me->eq, &ev);
}

/* Forward declaration — check_stops needs me_submit_order */
static void check_stops(me_engine_t *me);

static void emit_trade(me_engine_t      *me,
                       ob_order_id_t     aggressor_id,
                       ob_side_t         aggressor_side,
                       const ob_match_t *m)
{
    me_trade_t trade = {
        .trade_id       = ++me->trade_seq,
        .aggressor_id   = aggressor_id,
        .resting_id     = m->resting_id,
        .aggressor_side = aggressor_side,
        .price          = m->price,
        .qty            = m->qty,
        .ts             = m->ts,
    };

    me_event_t ev = {
        .type     = ME_EVENT_TRADE,
        .ts       = m->ts,
        .order_id = aggressor_id,
        .trade    = trade,
    };

    eq_push(&me->eq, &ev);
    me->total_trades++;
    me->total_qty_traded += m->qty;

    /* Update last trade price — used by stop triggers */
    me->last_trade_price = m->price;
}

static void maybe_emit_tob(me_engine_t *me)
{
    me_top_of_book_t tob = me_top_of_book(me);

    if (tob.bid_price == me->last_tob.bid_price &&
        tob.bid_qty   == me->last_tob.bid_qty   &&
        tob.ask_price == me->last_tob.ask_price &&
        tob.ask_qty   == me->last_tob.ask_qty)
        return;

    me_event_t ev = {
        .type = ME_EVENT_BOOK_UPDATED,
        .ts   = me_now(),
        .tob  = tob,
    };
    eq_push(&me->eq, &ev);
    me->last_tob = tob;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STOP POOL — dormant stop orders stored outside the book
 *
 * Stops are scanned after every trade. When last_trade_price crosses the
 * trigger, the stop activates: STOP → market order, STOP_LIMIT → limit.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Find an empty slot in the stop pool. Returns index or -1 if full. */
static int stop_pool_find_free(me_engine_t *me)
{
    for (uint32_t i = 0; i < ME_MAX_STOPS; i++)
        if (!me->stops[i].active) return (int)i;
    return -1;
}

/* Check all active stops against last_trade_price and trigger any that fire.
 * A triggered stop re-enters as a regular order via me_submit_order. */
static void check_stops(me_engine_t *me)
{
    ob_price_t ltp = me->last_trade_price;
    if (ltp == 0) return;   /* no trades yet */

    for (uint32_t i = 0; i < ME_MAX_STOPS; i++) {
        me_stop_order_t *s = &me->stops[i];
        if (!s->active) continue;

        /* Trigger condition (industry standard: last trade price):
         *   Buy stop:  triggers when ltp >= trigger_price (price rising)
         *   Sell stop: triggers when ltp <= trigger_price (price falling) */
        bool triggered = false;
        if (s->side == OB_SIDE_BID && ltp >= s->trigger_price)
            triggered = true;
        if (s->side == OB_SIDE_ASK && ltp <= s->trigger_price)
            triggered = true;

        if (!triggered) continue;

        /* Deactivate before re-entry to avoid infinite recursion */
        s->active = false;
        me->stop_count--;

        /* Emit stop triggered event */
        emit_order_event(me, ME_EVENT_STOP_TRIGGERED,
                         s->id, s->side, s->orig_type,
                         s->trigger_price, s->qty, s->qty, 0, NULL);

        /* Re-enter as a live order */
        me_result_t r;
        if (s->orig_type == ME_ORDER_STOP) {
            /* Stop-market: enter as market order */
            me_submit_order(me, ME_ORDER_MARKET, s->side, 0, s->qty, &r);
        } else {
            /* Stop-limit: enter as limit order at the saved limit price */
            me_submit_order(me, ME_ORDER_LIMIT, s->side,
                            s->limit_price, s->qty, &r);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API implementation
 * ═══════════════════════════════════════════════════════════════════════════ */

void me_init(me_engine_t   *me,
             const char    *symbol,
             me_handler_fn  handler,
             void          *ctx)
{
    memset(me, 0, sizeof(*me));
    ob_init(&me->book, symbol);
    me->handler     = handler;
    me->handler_ctx = ctx;
}

void me_destroy(me_engine_t *me)
{
    ob_destroy(&me->book);
}

/* ─── me_submit_order ───────────────────────────────────────────────────── */

void me_submit_order(me_engine_t     *me,
                     me_order_type_t  type,
                     ob_side_t        side,
                     ob_price_t       price,
                     ob_qty_t         qty,
                     me_result_t     *out)
{
    memset(out, 0, sizeof(*out));

    /* ── Step 1: Pre-process ─────────────────────────────────────────── */
    order_request_t req;
    orders_preprocess(&me->book, type, side, price, qty, &req);

    if (req.flow == FLOW_REJECT) {
        me->orders_rejected++;
        emit_order_event(me, ME_EVENT_ORDER_REJECTED,
                         0, side, type, price, qty, qty, 0, req.err_msg);
        out->status = OB_STATUS_REJECTED;
        maybe_emit_tob(me);
        return;
    }

    /* ── Step 2: Submit to the book ──────────────────────────────────── */
    ob_order_id_t id = 0;
    uint32_t      mc = 0;
    ob_match_t    match_scratch[OB_MAX_ORDERS];

    ob_status_t st = ob_add_order(&me->book, side, req.eff_price, qty,
                                  req.expiry_ts,
                                  &id, match_scratch, &mc);
    out->id = id;

    /* ── Step 3: Compute fill totals ─────────────────────────────────── */
    ob_qty_t qty_filled = 0;
    ob_qty_t vwap_num   = 0;

    for (uint32_t i = 0; i < mc; ++i) {
        qty_filled += match_scratch[i].qty;
        vwap_num   += (uint64_t)match_scratch[i].price * match_scratch[i].qty;
        emit_trade(me, id, side, &match_scratch[i]);
    }

    ob_qty_t qty_remain = qty - qty_filled;
    out->fills      = mc;
    out->qty_filled = qty_filled;
    out->qty_remain = qty_remain;
    out->avg_price  = (qty_filled > 0)
                      ? (ob_price_t)((vwap_num + qty_filled / 2) / qty_filled)
                      : 0;

    /* ── Step 4: IOC — cancel any unfilled remainder ─────────────────── */
    if (type == ME_ORDER_IOC && qty_remain > 0 && id != 0) {
        ob_cancel_order(&me->book, id);
        qty_remain      = 0;
        out->qty_remain = 0;
        emit_order_event(me, ME_EVENT_ORDER_CANCELLED,
                         id, side, type, price, qty,
                         0, qty_filled, "IOC remainder cancelled");
    }

    /* ── Step 5: Emit lifecycle event ────────────────────────────────── */
    if (st == OB_STATUS_REJECTED) {
        me->orders_rejected++;
        emit_order_event(me, ME_EVENT_ORDER_REJECTED,
                         id, side, type, price, qty, qty, 0,
                         "book capacity reached");
    } else {
        me->orders_accepted++;
        if (qty_remain == 0) {
            out->status = OB_STATUS_FILLED;
            emit_order_event(me, ME_EVENT_ORDER_FILLED,
                             id, side, type, price, qty,
                             0, qty_filled, NULL);
        } else if (qty_filled > 0) {
            out->status = OB_STATUS_PARTIAL;
            emit_order_event(me, ME_EVENT_ORDER_PARTIAL,
                             id, side, type, price, qty,
                             qty_remain, qty_filled, NULL);
        } else {
            out->status = OB_STATUS_OK;
            emit_order_event(me, ME_EVENT_ORDER_ACCEPTED,
                             id, side, type, price, qty,
                             qty, 0, NULL);
        }
    }

    maybe_emit_tob(me);

    /* ── Step 6: Check if any stops were triggered by this trade ──────── */
    if (mc > 0)
        check_stops(me);
}

/* ─── me_submit_stop ────────────────────────────────────────────────────── */

void me_submit_stop(me_engine_t     *me,
                    me_order_type_t  type,
                    ob_side_t        side,
                    ob_price_t       trigger_price,
                    ob_price_t       limit_price,
                    ob_qty_t         qty,
                    uint64_t         ttl_ns,
                    me_result_t     *out)
{
    memset(out, 0, sizeof(*out));

    /* Validate */
    if (qty == 0 || side >= OB_SIDE_COUNT || trigger_price <= 0) {
        me->orders_rejected++;
        emit_order_event(me, ME_EVENT_ORDER_REJECTED,
                         0, side, type, trigger_price, qty, qty, 0,
                         "invalid stop params");
        out->status = OB_STATUS_REJECTED;
        return;
    }
    if (type == ME_ORDER_STOP && limit_price != 0) {
        me->orders_rejected++;
        emit_order_event(me, ME_EVENT_ORDER_REJECTED,
                         0, side, type, trigger_price, qty, qty, 0,
                         "stop-market must have limit_price = 0");
        out->status = OB_STATUS_REJECTED;
        return;
    }
    if (type == ME_ORDER_STOP_LIMIT && limit_price <= 0) {
        me->orders_rejected++;
        emit_order_event(me, ME_EVENT_ORDER_REJECTED,
                         0, side, type, trigger_price, qty, qty, 0,
                         "stop-limit must have limit_price > 0");
        out->status = OB_STATUS_REJECTED;
        return;
    }

    /* Find free slot */
    int slot = stop_pool_find_free(me);
    if (slot < 0) {
        me->orders_rejected++;
        emit_order_event(me, ME_EVENT_ORDER_REJECTED,
                         0, side, type, trigger_price, qty, qty, 0,
                         "stop pool full");
        out->status = OB_STATUS_REJECTED;
        return;
    }

    /* Assign ID from the book's sequence (keeps IDs globally unique) */
    ob_order_id_t id = me->book.next_order_id++;

    me->stops[slot] = (me_stop_order_t){
        .id            = id,
        .side          = side,
        .orig_type     = type,
        .trigger_price = trigger_price,
        .limit_price   = limit_price,
        .qty           = qty,
        .expiry_ts     = (ttl_ns > 0) ? me_now() + ttl_ns : 0,
        .active        = true,
    };
    me->stop_count++;

    out->id     = id;
    out->status = OB_STATUS_OK;
    me->orders_accepted++;

    emit_order_event(me, ME_EVENT_ORDER_ACCEPTED,
                     id, side, type, trigger_price, qty, qty, 0, NULL);
}

/* ─── me_cancel_stop ────────────────────────────────────────────────────── */

bool me_cancel_stop(me_engine_t *me, ob_order_id_t id)
{
    for (uint32_t i = 0; i < ME_MAX_STOPS; i++) {
        me_stop_order_t *s = &me->stops[i];
        if (s->active && s->id == id) {
            s->active = false;
            me->stop_count--;
            emit_order_event(me, ME_EVENT_ORDER_CANCELLED,
                             id, s->side, s->orig_type,
                             s->trigger_price, s->qty, 0, 0, NULL);
            return true;
        }
    }
    return false;
}

/* ─── me_expire_orders ──────────────────────────────────────────────────── */

uint32_t me_expire_orders(me_engine_t *me, ob_ts_t now)
{
    uint32_t expired = 0;

    /* Sweep expired stops */
    for (uint32_t i = 0; i < ME_MAX_STOPS; i++) {
        me_stop_order_t *s = &me->stops[i];
        if (s->active && s->expiry_ts > 0 && s->expiry_ts <= now) {
            s->active = false;
            me->stop_count--;
            emit_order_event(me, ME_EVENT_ORDER_CANCELLED,
                             s->id, s->side, s->orig_type,
                             s->trigger_price, s->qty, 0, 0,
                             "expired");
            expired++;
        }
    }

    /* Sweep expired book orders (day orders with expiry_ts set) */
    for (uint16_t i = 0; i < OB_MAX_ORDERS; i++) {
        ob_order_t *o = &me->book.order_pool[i];
        if (o->id != 0 && o->expiry_ts > 0 && o->expiry_ts <= now) {
            ob_order_id_t oid = o->id;
            ob_side_t     sd  = o->side;
            ob_price_t    px  = o->price;
            ob_qty_t      qt  = o->qty;
            ob_cancel_order(&me->book, oid);
            emit_order_event(me, ME_EVENT_ORDER_CANCELLED,
                             oid, sd, ME_ORDER_LIMIT, px, qt, 0, 0,
                             "day order expired");
            expired++;
        }
    }

    if (expired > 0) maybe_emit_tob(me);
    return expired;
}

/* ─── me_cancel_order ───────────────────────────────────────────────────── */

bool me_cancel_order(me_engine_t *me, ob_order_id_t id)
{
    ob_order_t snap;
    if (!ob_get_order(&me->book, id, &snap)) return false;

    ob_status_t st = ob_cancel_order(&me->book, id);
    if (st != OB_STATUS_OK) return false;

    emit_order_event(me, ME_EVENT_ORDER_CANCELLED,
                     id, snap.side, ME_ORDER_LIMIT, snap.price,
                     snap.qty, 0, snap.qty_remain, NULL);
    maybe_emit_tob(me);
    return true;
}

/* ─── me_modify_order ───────────────────────────────────────────────────── */

bool me_modify_order(me_engine_t  *me,
                     ob_order_id_t id,
                     ob_price_t    new_price,
                     ob_qty_t      new_qty)
{
    ob_order_t snap;
    if (!ob_get_order(&me->book, id, &snap)) return false;

    ob_status_t st = ob_modify_order(&me->book, id, new_price, new_qty);
    if (st != OB_STATUS_OK) return false;

    emit_order_event(me, ME_EVENT_ORDER_MODIFIED,
                     id, snap.side, ME_ORDER_LIMIT, new_price,
                     snap.qty, new_qty, 0, NULL);
    maybe_emit_tob(me);
    return true;
}

/* ─── me_run_events ─────────────────────────────────────────────────────── */

uint32_t me_run_events(me_engine_t *me)
{
    if (!me->handler) return 0;
    uint32_t   n = 0;
    me_event_t ev;
    while (eq_pop(&me->eq, &ev)) {
        me->handler(&ev, me->handler_ctx);
        n++;
    }
    return n;
}

/* ─── QUERIES ───────────────────────────────────────────────────────────── */

me_top_of_book_t me_top_of_book(const me_engine_t *me)
{
    me_top_of_book_t tob = {0};
    tob.bid_price = ob_best_bid(&me->book);
    tob.ask_price = ob_best_ask(&me->book);
    if (tob.bid_price > 0)
        tob.bid_qty = ob_depth(&me->book, OB_SIDE_BID, 0);
    if (tob.ask_price > 0)
        tob.ask_qty = ob_depth(&me->book, OB_SIDE_ASK, 0);
    tob.spread = ob_spread(&me->book);
    tob.mid    = ob_mid_price(&me->book);
    return tob;
}

bool me_is_crossed(const me_engine_t *me,
                   ob_side_t          side,
                   ob_price_t         price)
{
    return orders_crosses(&me->book, side, price, ME_ORDER_LIMIT);
}

/* ─── STATS ─────────────────────────────────────────────────────────────── */

void me_print_stats(const me_engine_t *me)
{
    const ob_book_t *b = &me->book;
    printf("\n+===============================================+\n");
    printf("|  Matching Engine Stats -- %-18s |\n", b->symbol);
    printf("+===============================================+\n");
    printf("|  Orders accepted : %-25llu |\n",
           (unsigned long long)me->orders_accepted);
    printf("|  Orders rejected : %-25llu |\n",
           (unsigned long long)me->orders_rejected);
    printf("|  Total trades    : %-25llu |\n",
           (unsigned long long)me->total_trades);
    printf("|  Total qty traded: %-25llu |\n",
           (unsigned long long)me->total_qty_traded);
    printf("|  Active stops    : %-25u |\n",
           me->stop_count);
    printf("|  Events dropped  : %-25u |\n",
           me->eq.dropped);
    printf("+-----------------------------------------------+\n");
    printf("|  Best bid: %-10.4f  Best ask: %-10.4f |\n",
           OB_PRICE_TO_DOUBLE(ob_best_bid(b)),
           OB_PRICE_TO_DOUBLE(ob_best_ask(b)));
    printf("|  Spread:   %-10.4f  Mid:      %-10.4f |\n",
           OB_PRICE_TO_DOUBLE(ob_spread(b)),
           OB_PRICE_TO_DOUBLE(ob_mid_price(b)));
    printf("+===============================================+\n\n");
}