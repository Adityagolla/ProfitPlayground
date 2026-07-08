/* Enable C99 printf format specifiers on MinGW */
#if defined(__MINGW32__) || defined(__MINGW64__)
#define __USE_MINGW_ANSI_STDIO 1
#endif

/*
 * pipeline_demo.c — End-to-end demonstration of the trade lifecycle
 *
 * Walks through:
 *   1. Initialise pipeline + 2 accounts + WAL
 *   2. Subscribe ML / UI / Analytics-style listeners to the bus
 *   3. Submit several orders covering: LIMIT, MARKET, IOC, FOK, STOP
 *   4. Demonstrate idempotency, kill-switch, fat-finger, position limits
 *   5. Print final stats and per-account P&L
 *
 * Build (MinGW):
 *   gcc -O2 -Wall -o pipeline_demo.exe pipeline_demo.c pipeline.c gateway.c \
 *       risk.c portfolio.c event_bus.c matching_engine.c orders.c orderbook.c
 */

#include "pipeline.h"
#include "orderbook.h"
#include <stdio.h>
#include <inttypes.h>

/* ─── Subscribers ───────────────────────────────────────────────────────── */

/* ui_handler — pretty-print every order/trade/book event to stdout. */
static void ui_handler(const bus_msg_t *msg, void *ctx)
{
    (void)ctx;
    if (msg->topic == BUS_TOPIC_TRADE) {
        const me_event_t *ev = msg->payload;
        printf("  [UI] TRADE  trade_id=%llu  price=%lld  qty=%llu  acct=%u\n",
               (unsigned long long)ev->trade.trade_id,
               (long long)ev->trade.price,
               (unsigned long long)ev->trade.qty,
               msg->account_id);
    } else if (msg->topic == BUS_TOPIC_BOOK) {
        const me_event_t *ev = msg->payload;
        printf("  [UI] BOOK   bid=%lld(%llu)  ask=%lld(%llu)\n",
               (long long)ev->tob.bid_price, (unsigned long long)ev->tob.bid_qty,
               (long long)ev->tob.ask_price, (unsigned long long)ev->tob.ask_qty);
    } else if (msg->topic == BUS_TOPIC_ORDER) {
        const me_event_t *ev = msg->payload;
        const char *t = (ev->type == ME_EVENT_ORDER_ACCEPTED)  ? "ACCEPT"
                       : (ev->type == ME_EVENT_ORDER_FILLED)   ? "FILLED"
                       : (ev->type == ME_EVENT_ORDER_PARTIAL)  ? "PARTIAL"
                       : (ev->type == ME_EVENT_ORDER_REJECTED) ? "REJECT"
                       : (ev->type == ME_EVENT_ORDER_CANCELLED)? "CANCEL"
                       : (ev->type == ME_EVENT_STOP_TRIGGERED) ? "STOPHIT"
                       : "ORDER";
        printf("  [UI] %-7s id=%llu side=%d qty=%llu->%llu filled=%llu\n",
               t,
               (unsigned long long)ev->order_id, (int)ev->side,
               (unsigned long long)ev->qty_original,
               (unsigned long long)ev->qty_remain,
               (unsigned long long)ev->qty_filled);
    } else if (msg->topic == BUS_TOPIC_GATEWAY) {
        const gw_ack_t *ack = msg->payload;
        /* We mirror two payload shapes on this topic — gw_ack_t (pointer
         * always non-NULL after pipeline_submit) and downstream reject
         * strings. Heuristic: gw_ack_t will have status <= 4. */
        if (ack && ack->status <= GW_ACK_KILLED && ack->ingress_ts_ns > 0) {
            const char *st = (ack->status == GW_ACK_ACCEPTED) ? "ACCEPTED"
                           : (ack->status == GW_ACK_REJECTED) ? "REJECTED"
                           : (ack->status == GW_ACK_DUPLICATE) ? "DUP"
                           : (ack->status == GW_ACK_THROTTLED) ? "THROTTLED"
                           : "KILLED";
            printf("  [GW] %-9s server_id=%llu  reason=%s\n",
                   st,
                   (unsigned long long)ack->server_order_id,
                   ack->reject_reason ? ack->reject_reason : "-");
        } else if (ack) {
            printf("  [GW] downstream-reject: %s\n", (const char *)ack);
        }
    }
}

/* analytics_handler — keep simple counters; in real life this aggregates VWAP. */
typedef struct { uint64_t trades; uint64_t qty; } analytics_t;
static void analytics_handler(const bus_msg_t *msg, void *ctx)
{
    if (msg->topic != BUS_TOPIC_TRADE) return;
    analytics_t *a = ctx;
    const me_event_t *ev = msg->payload;
    a->trades++;
    a->qty += ev->trade.qty;
}

/* ml_handler — placeholder: would push features into a circular buffer. */
static void ml_handler(const bus_msg_t *msg, void *ctx)
{
    (void)msg; (void)ctx;
    /* Real impl: extract (price, qty, side, ts) → feature vector → ring */
}

/* ─── Helpers to build raw messages ─────────────────────────────────────── */

static gw_raw_msg_t mk_new(uint32_t acct, uint64_t coid,
                           me_order_type_t type, ob_side_t side,
                           ob_price_t price, ob_qty_t qty)
{
    gw_raw_msg_t m = {0};
    m.source = GW_SRC_API;
    m.action = GW_ACT_NEW;
    m.account_id      = acct;
    m.client_order_id = coid;
    m.type   = type;
    m.side   = side;
    m.price  = price;
    m.qty    = qty;
    return m;
}

static gw_raw_msg_t mk_stop(uint32_t acct, uint64_t coid, ob_side_t side,
                            ob_price_t trigger, ob_price_t limit_px,
                            ob_qty_t qty)
{
    gw_raw_msg_t m = {0};
    m.source = GW_SRC_API;
    m.action = GW_ACT_NEW;
    m.account_id      = acct;
    m.client_order_id = coid;
    m.type   = limit_px > 0 ? ME_ORDER_STOP_LIMIT : ME_ORDER_STOP;
    m.side   = side;
    m.price  = limit_px;
    m.trigger_price = trigger;
    m.qty    = qty;
    return m;
}

/* ─── Section runner ────────────────────────────────────────────────────── */

#define SECTION(name) printf("\n=== %s ===\n", name)

int main(void)
{
    /* static, not stack-local: pipeline_t is well over a megabyte now that
     * PIPELINE_MAX_ID_MAP is generous, and would overflow the default
     * thread stack if placed on it. */
    static pipeline_t pl;
    pipeline_init(&pl, "DEMO");
    pipeline_set_wal(&pl, "pipeline.wal");

    /* Two accounts: maker (1) and taker (2). */
    pipeline_add_account(&pl, 1, /*cash*/ 10000000, /*max_pos*/ 1000,
                         /*max_notional*/ 5000000);
    pipeline_add_account(&pl, 2, /*cash*/ 10000000, /*max_pos*/ 1000,
                         /*max_notional*/ 5000000);

    analytics_t analytics = {0};
    pipeline_subscribe(&pl, BUS_TOPIC_ORDER | BUS_TOPIC_TRADE |
                            BUS_TOPIC_BOOK  | BUS_TOPIC_GATEWAY,
                       ui_handler, NULL);
    pipeline_subscribe(&pl, BUS_TOPIC_TRADE, analytics_handler, &analytics);
    pipeline_subscribe(&pl, BUS_TOPIC_TRADE, ml_handler, NULL);

    /* ── 1. Maker rests a limit ask ─────────────────────────────────── */
    SECTION("1. Maker (acct=1) places LIMIT ASK 100 @ 10000");
    {
        gw_raw_msg_t m = mk_new(1, 1001, ME_ORDER_LIMIT, OB_SIDE_ASK, 10000, 100);
        pipeline_submit(&pl, &m);
        pipeline_step(&pl);
    }

    /* ── 2. Taker hits with a market buy ────────────────────────────── */
    SECTION("2. Taker (acct=2) MARKET BUY 30");
    {
        gw_raw_msg_t m = mk_new(2, 2001, ME_ORDER_MARKET, OB_SIDE_BID, 0, 30);
        pipeline_submit(&pl, &m);
        pipeline_step(&pl);
    }

    /* ── 3. IOC: fill what's available, cancel rest ─────────────────── */
    SECTION("3. Taker IOC BUY 200 @ 10000 (only 70 left)");
    {
        gw_raw_msg_t m = mk_new(2, 2002, ME_ORDER_IOC, OB_SIDE_BID, 10000, 200);
        pipeline_submit(&pl, &m);
        pipeline_step(&pl);
    }

    /* ── 4. FOK: not enough liquidity → rejected at ME ──────────────── */
    SECTION("4. Taker FOK BUY 500 @ 10000 (book empty → reject)");
    {
        gw_raw_msg_t m = mk_new(2, 2003, ME_ORDER_FOK, OB_SIDE_BID, 10000, 500);
        pipeline_submit(&pl, &m);
        pipeline_step(&pl);
    }

    /* ── 5. Idempotent retry of the same client_order_id ────────────── */
    SECTION("5. Duplicate client_order_id (replay of #1)");
    {
        gw_raw_msg_t m = mk_new(1, 1001, ME_ORDER_LIMIT, OB_SIDE_ASK, 10000, 100);
        gw_ack_t a = pipeline_submit(&pl, &m);
        printf("  caller sees status=%d server_id=%llu reason=%s\n",
               a.status, (unsigned long long)a.server_order_id,
               a.reject_reason);
        pipeline_step(&pl);
    }

    /* ── 6. Stop loss: maker rests a sell stop, then market trades down ── */
    SECTION("6. Stop-loss: acct=1 SELL STOP trigger=9500 qty=20");
    {
        /* Build context: maker has a long position from earlier? Not
         * really — the ME treats all new orders as fresh. We just
         * demonstrate the trigger mechanic. First, rebuild some bid
         * liquidity at 9500 so the stop has something to hit. */
        gw_raw_msg_t bid = mk_new(2, 2010, ME_ORDER_LIMIT, OB_SIDE_BID, 9500, 50);
        pipeline_submit(&pl, &bid);
        pipeline_step(&pl);

        /* Now place a SELL STOP that triggers at 9500. */
        gw_raw_msg_t st = mk_stop(1, 1010, OB_SIDE_ASK,
                                  /*trigger*/ 9500, /*limit*/ 0, /*qty*/ 20);
        pipeline_submit(&pl, &st);
        pipeline_step(&pl);

        /* A small print trade at 9500 to fire the stop. */
        gw_raw_msg_t hit = mk_new(2, 2011, ME_ORDER_LIMIT, OB_SIDE_BID, 9500, 1);
        pipeline_submit(&pl, &hit);
        pipeline_step(&pl);

        /* Cause an ASK trade at 9500 (we already sit on the bid side). */
        gw_raw_msg_t cross = mk_new(1, 1011, ME_ORDER_LIMIT, OB_SIDE_ASK, 9500, 1);
        pipeline_submit(&pl, &cross);
        pipeline_step(&pl);
    }

    /* ── 7. Fat-finger guard ────────────────────────────────────────── */
    SECTION("7. Fat-finger: BUY @ 99999 (>10% from last)");
    {
        gw_raw_msg_t m = mk_new(2, 2099, ME_ORDER_LIMIT, OB_SIDE_BID, 99999, 1);
        pipeline_submit(&pl, &m);
        pipeline_step(&pl);
    }

    /* ── 8. Kill-switch ─────────────────────────────────────────────── */
    SECTION("8. Kill-switch ON, then a NEW order");
    {
        gateway_set_kill_switch(&pl.gateway, true);
        gw_raw_msg_t m = mk_new(2, 2100, ME_ORDER_LIMIT, OB_SIDE_BID, 9500, 1);
        gw_ack_t a = pipeline_submit(&pl, &m);
        printf("  ack.status=%d reason=%s\n", a.status, a.reject_reason);
        gateway_set_kill_switch(&pl.gateway, false);
    }

    /* ── 9. Stats and portfolios ────────────────────────────────────── */
    pipeline_print_stats(&pl);

    for (uint32_t i = 1; i <= 2; i++) {
        portfolio_t *p = pipeline_account(&pl, i);
        if (!p) continue;
        printf("\n  Account %u  cash=%lld  net_qty=%lld  avg_px=%lld"
               "  realised=%lld  unreal@last=%lld\n",
               p->account_id,
               (long long)p->cash,
               (long long)p->net_qty,
               (long long)p->avg_price,
               (long long)p->realised_pnl,
               (long long)portfolio_unrealised_pnl(p, pl.engine.last_trade_price));
    }

    printf("\n  Analytics: %llu trades, %llu total qty\n",
           (unsigned long long)analytics.trades,
           (unsigned long long)analytics.qty);

    pipeline_set_wal(&pl, NULL);   /* close WAL */
    me_destroy(&pl.engine);
    return 0;
}
