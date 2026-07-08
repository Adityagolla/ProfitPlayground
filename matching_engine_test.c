/*
 * me_test.c — Full test suite for the matching engine
 *
 * Compile:
 *   gcc -Wall -Wextra -O2 -o me_test me_test.c matching_engine.c order_book.c && ./me_test
 */
/* Enable proper C99 printf format specifiers (%llu, %zu) on MinGW */
#if defined(__MINGW32__) || defined(__MINGW64__)
#define __USE_MINGW_ANSI_STDIO 1
#endif

#include "matching_engine.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

/* ─── ANSI colours ──────────────────────────────────────────────────────── */
#define GRN "\033[32m"
#define RED "\033[31m"
#define YLW "\033[33m"
#define CYN "\033[36m"
#define DIM "\033[2m"
#define RST "\033[0m"

/* ─── Global event log (collected by handler) ───────────────────────────── */

#define MAX_LOG 512
static me_event_t g_log[MAX_LOG];
static uint32_t   g_log_n = 0;

static void log_handler(const me_event_t *ev, void *ctx)
{
    (void)ctx;
    if (g_log_n < MAX_LOG) g_log[g_log_n++] = *ev;
}

static void reset_log(void) { g_log_n = 0; }

/* Count events of a given type in the log */
static uint32_t count_events(me_event_type_t t)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_log_n; ++i)
        if (g_log[i].type == t) n++;
    return n;
}

/* Print the event log */
static void print_log(void)
{
    for (uint32_t i = 0; i < g_log_n; ++i) {
        const me_event_t *ev = &g_log[i];
        if (ev->type == ME_EVENT_TRADE) {
            printf("  " CYN "[TRADE]" RST
                   " trade#%llu  agg=#%llu  rest=#%llu"
                   "  price=%.4f  qty=%llu\n",
                   (unsigned long long)ev->trade.trade_id,
                   (unsigned long long)ev->trade.aggressor_id,
                   (unsigned long long)ev->trade.resting_id,
                   OB_PRICE_TO_DOUBLE(ev->trade.price),
                   (unsigned long long)ev->trade.qty);
        } else if (ev->type == ME_EVENT_BOOK_UPDATED) {
            printf("  " DIM "[TOB] bid=%.4f x %llu  ask=%.4f x %llu"
                   "  spread=%.4f" RST "\n",
                   OB_PRICE_TO_DOUBLE(ev->tob.bid_price),
                   (unsigned long long)ev->tob.bid_qty,
                   OB_PRICE_TO_DOUBLE(ev->tob.ask_price),
                   (unsigned long long)ev->tob.ask_qty,
                   OB_PRICE_TO_DOUBLE(ev->tob.spread));
        } else {
            const char *col = (ev->type == ME_EVENT_ORDER_REJECTED) ? RED :
                              (ev->type == ME_EVENT_ORDER_FILLED)   ? GRN :
                              (ev->type == ME_EVENT_ORDER_PARTIAL)  ? YLW : "";
            printf("  %s[%s]%s #%llu %s %s"
                   "  price=%.4f  orig=%llu  filled=%llu  remain=%llu",
                   col, me_event_name(ev->type), RST,
                   (unsigned long long)ev->order_id,
                   ev->side == OB_SIDE_BID ? "BID" : "ASK",
                   me_order_type_name(ev->order_type),
                   OB_PRICE_TO_DOUBLE(ev->price),
                   (unsigned long long)ev->qty_original,
                   (unsigned long long)ev->qty_filled,
                   (unsigned long long)ev->qty_remain);
            if (ev->type == ME_EVENT_ORDER_REJECTED)
                printf("  reason=\"%s\"", ev->reject_reason);
            printf("\n");
        }
    }
}

/* ─── Test helpers ──────────────────────────────────────────────────────── */

#define SECTION(name) \
    printf("\n" CYN "══ " name " " RST "\n")

#define PASS() printf("  " GRN "✓ PASS" RST "\n")
#define FAIL(msg) do { printf("  " RED "✗ FAIL: " msg RST "\n"); } while(0)

/* ══════════════════════════════════════════════════════════════════════════
 * Test 1 — Limit order: no cross → rests in book
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_limit_resting(void)
{
    SECTION("Test 1: Limit order — no cross, order rests");

    me_engine_t me;
    me_init(&me, "AAPL", log_handler, NULL);
    reset_log();

    me_result_t r;
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_BID,
                    OB_PRICE_FROM_DOUBLE(150.00), 200, &r);
    me_run_events(&me);

    printf("  Submitted BID LIMIT 200 @ 150.00\n");
    print_log();

    assert(r.status     == OB_STATUS_OK);
    assert(r.qty_filled == 0);
    assert(r.qty_remain == 200);
    assert(r.fills      == 0);
    assert(count_events(ME_EVENT_ORDER_ACCEPTED) == 1);
    assert(count_events(ME_EVENT_BOOK_UPDATED)   == 1);
    assert(ob_best_bid(&me.book) == OB_PRICE_FROM_DOUBLE(150.00));

    PASS();
    me_destroy(&me);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 2 — Limit order: full cross → full fill
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_limit_full_cross(void)
{
    SECTION("Test 2: Limit order — full cross");

    me_engine_t me;
    me_init(&me, "TSLA", log_handler, NULL);

    /* Rest an ask */
    me_result_t r;
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_ASK,
                    OB_PRICE_FROM_DOUBLE(200.00), 100, &r);
    me_run_events(&me);

    reset_log();

    /* Aggressive bid crosses it */
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_BID,
                    OB_PRICE_FROM_DOUBLE(201.00), 100, &r);
    me_run_events(&me);

    printf("  Resting ASK: sell 100 @ 200.00\n");
    printf("  Aggressive BID: buy 100 @ 201.00\n");
    print_log();

    assert(r.status      == OB_STATUS_FILLED);
    assert(r.qty_filled  == 100);
    assert(r.qty_remain  == 0);
    assert(r.fills       == 1);
    /* Execution at resting price (200.00) */
    assert(r.avg_price   == OB_PRICE_FROM_DOUBLE(200.00));
    assert(count_events(ME_EVENT_TRADE)        == 1);
    assert(count_events(ME_EVENT_ORDER_FILLED) == 1);
    assert(ob_best_ask(&me.book) == 0);  /* ask side empty */

    PASS();
    me_destroy(&me);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 3 — Limit order: partial cross, remainder rests
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_limit_partial_cross(void)
{
    SECTION("Test 3: Limit order — partial cross, remainder rests");

    me_engine_t me;
    me_init(&me, "MSFT", log_handler, NULL);

    me_result_t r;
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_ASK,
                    OB_PRICE_FROM_DOUBLE(300.00), 50, &r);

    reset_log();

    /* Buy 200, only 50 available */
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_BID,
                    OB_PRICE_FROM_DOUBLE(300.00), 200, &r);
    me_run_events(&me);

    printf("  Resting ASK: sell 50 @ 300.00\n");
    printf("  Aggressive BID: buy 200 @ 300.00 (only 50 available)\n");
    print_log();

    assert(r.status     == OB_STATUS_PARTIAL);
    assert(r.qty_filled == 50);
    assert(r.qty_remain == 150);
    assert(r.fills      == 1);
    assert(count_events(ME_EVENT_TRADE)          == 1);
    assert(count_events(ME_EVENT_ORDER_PARTIAL)  == 1);
    /* Remainder now rests as the best bid */
    assert(ob_best_bid(&me.book) == OB_PRICE_FROM_DOUBLE(300.00));

    PASS();
    me_destroy(&me);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 4 — Market order: sweeps multiple levels
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_market_sweep(void)
{
    SECTION("Test 4: Market order — sweeps multiple ask levels");

    me_engine_t me;
    me_init(&me, "NVDA", log_handler, NULL);

    me_result_t r;
    /* Three ask levels */
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_ASK,
                    OB_PRICE_FROM_DOUBLE(400.00),  30, &r);
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_ASK,
                    OB_PRICE_FROM_DOUBLE(400.10),  50, &r);
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_ASK,
                    OB_PRICE_FROM_DOUBLE(400.20), 100, &r);

    reset_log();

    /* Market buy for 120 */
    me_submit_order(&me, ME_ORDER_MARKET, OB_SIDE_BID, 0, 120, &r);
    me_run_events(&me);

    printf("  Resting ASKs: 30@400.00, 50@400.10, 100@400.20\n");
    printf("  Market BID for 120\n");
    print_log();

    assert(r.status     == OB_STATUS_FILLED);
    assert(r.qty_filled == 120);
    assert(r.qty_remain == 0);
    assert(r.fills      == 3);  /* 30 + 50 + 40 from three levels */
    /* Partial 400.20 still in book */
    assert(ob_best_ask(&me.book) == OB_PRICE_FROM_DOUBLE(400.20));

    PASS();
    me_destroy(&me);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 5 — IOC: partial fill, remainder cancelled
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_ioc_partial(void)
{
    SECTION("Test 5: IOC — partial fill, remainder cancelled (not resting)");

    me_engine_t me;
    me_init(&me, "AMZN", log_handler, NULL);

    me_result_t r;
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_ASK,
                    OB_PRICE_FROM_DOUBLE(180.00), 40, &r);

    reset_log();

    /* IOC buy for 100 — only 40 available */
    me_submit_order(&me, ME_ORDER_IOC, OB_SIDE_BID,
                    OB_PRICE_FROM_DOUBLE(181.00), 100, &r);
    me_run_events(&me);

    printf("  Resting ASK: sell 40 @ 180.00\n");
    printf("  IOC BID: buy 100 @ 181.00 (only 40 fillable)\n");
    print_log();

    assert(r.qty_filled == 40);
    assert(r.qty_remain == 0);    /* IOC cancels the remainder */
    assert(count_events(ME_EVENT_TRADE)           == 1);
    assert(count_events(ME_EVENT_ORDER_CANCELLED) >= 1);
    /* No resting bid should exist */
    assert(ob_best_bid(&me.book) == 0);

    PASS();
    me_destroy(&me);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 6 — FOK: book can fill → executes; can't fill → rejects
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_fok(void)
{
    SECTION("Test 6: FOK — fill-or-kill logic");

    me_engine_t me;
    me_init(&me, "META", log_handler, NULL);

    me_result_t r;
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_ASK,
                    OB_PRICE_FROM_DOUBLE(500.00), 100, &r);

    /* ── FOK for 50 → should fill (100 available) ── */
    reset_log();
    me_submit_order(&me, ME_ORDER_FOK, OB_SIDE_BID,
                    OB_PRICE_FROM_DOUBLE(500.00), 50, &r);
    me_run_events(&me);

    printf("  Resting ASK: sell 100 @ 500.00\n");
    printf("  FOK BID 50 @ 500.00 (100 available → should fill)\n");
    print_log();

    assert(r.status     == OB_STATUS_FILLED);
    assert(r.qty_filled == 50);

    /* ── FOK for 200 → should reject (only 50 left) ── */
    reset_log();
    me_submit_order(&me, ME_ORDER_FOK, OB_SIDE_BID,
                    OB_PRICE_FROM_DOUBLE(500.00), 200, &r);
    me_run_events(&me);

    printf("\n  FOK BID 200 @ 500.00 (only 50 available → should reject)\n");
    print_log();

    assert(r.status     == OB_STATUS_REJECTED);
    assert(r.qty_filled == 0);
    assert(count_events(ME_EVENT_ORDER_REJECTED) == 1);

    PASS();
    me_destroy(&me);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 7 — Price-Time priority (FIFO within a level)
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_price_time_priority(void)
{
    SECTION("Test 7: Price-Time priority");

    me_engine_t me;
    me_init(&me, "GOOG", log_handler, NULL);

    me_result_t r;
    /* Three asks at the same price — order A, B, C */
    ob_order_id_t id_a, id_b, id_c;

    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_ASK,
                    OB_PRICE_FROM_DOUBLE(140.00), 10, &r);
    id_a = r.id;
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_ASK,
                    OB_PRICE_FROM_DOUBLE(140.00), 20, &r);
    id_b = r.id;
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_ASK,
                    OB_PRICE_FROM_DOUBLE(140.00), 30, &r);
    id_c = r.id;

    /* Also a worse-price ask at 141.00 */
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_ASK,
                    OB_PRICE_FROM_DOUBLE(141.00), 100, &r);

    reset_log();

    /* Buy 35: should fill A(10) fully, B(20) fully, C(5) partially */
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_BID,
                    OB_PRICE_FROM_DOUBLE(141.00), 35, &r);
    me_run_events(&me);

    printf("  Resting ASKs @ 140.00: A=%llu(10), B=%llu(20), C=%llu(30)\n",
           (unsigned long long)id_a,
           (unsigned long long)id_b,
           (unsigned long long)id_c);
    printf("  Aggressive BID: buy 35 @ 141.00\n");
    print_log();

    assert(r.qty_filled == 35);
    assert(r.fills      == 3);

    /* Verify fill order: first fill = id_a, second = id_b */
    uint32_t ti = 0;
    for (uint32_t i = 0; i < g_log_n; ++i) {
        if (g_log[i].type == ME_EVENT_TRADE) {
            if (ti == 0) assert(g_log[i].trade.resting_id == id_a);
            if (ti == 1) assert(g_log[i].trade.resting_id == id_b);
            if (ti == 2) assert(g_log[i].trade.resting_id == id_c);
            ti++;
        }
    }
    assert(ti == 3);

    PASS();
    me_destroy(&me);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 8 — Cancel and Modify via engine API
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_cancel_modify(void)
{
    SECTION("Test 8: Cancel and Modify via engine API");

    me_engine_t me;
    me_init(&me, "SPY", log_handler, NULL);

    me_result_t r;
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_BID,
                    OB_PRICE_FROM_DOUBLE(450.00), 500, &r);
    ob_order_id_t id = r.id;

    reset_log();

    /* Modify price upward */
    bool ok = me_modify_order(&me, id,
                              OB_PRICE_FROM_DOUBLE(451.00), 500);
    me_run_events(&me);
    assert(ok);
    printf("  Modified #%llu → 500 @ 451.00\n", (unsigned long long)id);
    assert(ob_best_bid(&me.book) == OB_PRICE_FROM_DOUBLE(451.00));
    assert(count_events(ME_EVENT_ORDER_MODIFIED)  == 1);
    assert(count_events(ME_EVENT_BOOK_UPDATED)    >= 1);

    reset_log();

    /* Cancel */
    ok = me_cancel_order(&me, id);
    me_run_events(&me);
    assert(ok);
    printf("  Cancelled #%llu\n", (unsigned long long)id);
    print_log();
    assert(count_events(ME_EVENT_ORDER_CANCELLED) == 1);
    assert(ob_best_bid(&me.book) == 0);

    PASS();
    me_destroy(&me);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 9 — Validation: bad params rejected cleanly
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_validation(void)
{
    SECTION("Test 9: Validation — bad params");

    me_engine_t me;
    me_init(&me, "QQQ", log_handler, NULL);
    reset_log();

    me_result_t r;

    /* qty = 0 */
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_BID,
                    OB_PRICE_FROM_DOUBLE(100.00), 0, &r);
    me_run_events(&me);
    printf("  qty=0: "); print_log(); reset_log();
    assert(r.status == OB_STATUS_REJECTED);

    /* Limit price = 0 */
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_ASK, 0, 10, &r);
    me_run_events(&me);
    printf("  Limit price=0: "); print_log(); reset_log();
    assert(r.status == OB_STATUS_REJECTED);

    /* Market order with non-zero price */
    me_submit_order(&me, ME_ORDER_MARKET, OB_SIDE_BID,
                    OB_PRICE_FROM_DOUBLE(100.00), 10, &r);
    me_run_events(&me);
    printf("  Market with price!=0: "); print_log(); reset_log();
    assert(r.status == OB_STATUS_REJECTED);

    PASS();
    me_destroy(&me);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 10 — Full engine walkthrough with ob_print
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_full_scenario(void)
{
    SECTION("Test 10: Full trading scenario");

    me_engine_t me;
    me_init(&me, "BTC/USD", log_handler, NULL);

    me_result_t r;

    /* Build a realistic book */
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_BID,
                    OB_PRICE_FROM_DOUBLE(69500.00), 1000, &r);
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_BID,
                    OB_PRICE_FROM_DOUBLE(69450.00), 2500, &r);
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_BID,
                    OB_PRICE_FROM_DOUBLE(69400.00), 5000, &r);
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_ASK,
                    OB_PRICE_FROM_DOUBLE(69600.00), 800, &r);
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_ASK,
                    OB_PRICE_FROM_DOUBLE(69650.00), 1500, &r);
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_ASK,
                    OB_PRICE_FROM_DOUBLE(69700.00), 3000, &r);

    me_run_events(&me);
    reset_log();
    ob_print(&me.book, 5);

    /* Market sell sweeping bids */
    printf("  Market SELL for 4000 (sweeps 69500 + 69450 + partial 69400)\n");
    me_submit_order(&me, ME_ORDER_MARKET, OB_SIDE_ASK, 0, 4000, &r);
    me_run_events(&me);
    print_log();

    assert(r.qty_filled == 4000);
    assert(r.fills      == 3);

    ob_print(&me.book, 5);
    me_print_stats(&me);

    PASS();
    me_destroy(&me);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 11 — Stop-market triggers on price touch → fills as market
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_stop_market_trigger(void)
{
    SECTION("Test 11: Stop-market triggers on price touch");

    me_engine_t me;
    me_init(&me, "STOP1", log_handler, NULL);

    me_result_t r;

    /* Build some resting asks */
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_ASK,
                    OB_PRICE_FROM_DOUBLE(100.00), 200, &r);
    me_run_events(&me);

    /* Place a buy stop: trigger when price >= 100.00 */
    reset_log();
    me_submit_stop(&me, ME_ORDER_STOP, OB_SIDE_BID,
                   OB_PRICE_FROM_DOUBLE(100.00), 0, 50, 0, &r);
    me_run_events(&me);

    printf("  Submitted BUY STOP trigger=100.00 qty=50\n");
    assert(r.status == OB_STATUS_OK);
    assert(me.stop_count == 1);

    /* Now trigger: a trade at 100.00 fires the stop */
    reset_log();
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_BID,
                    OB_PRICE_FROM_DOUBLE(100.00), 10, &r);
    me_run_events(&me);

    printf("  Trade at 100.00 triggers the stop\n");
    print_log();

    /* Stop should have fired and filled as market */
    assert(me.stop_count == 0);
    assert(count_events(ME_EVENT_STOP_TRIGGERED) == 1);

    PASS();
    me_destroy(&me);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 12 — Stop-limit triggers → enters as limit, may rest
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_stop_limit_trigger(void)
{
    SECTION("Test 12: Stop-limit triggers → enters as limit");

    me_engine_t me;
    me_init(&me, "STOP2", log_handler, NULL);

    me_result_t r;

    /* Build resting asks at 105 */
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_ASK,
                    OB_PRICE_FROM_DOUBLE(105.00), 100, &r);
    me_run_events(&me);

    /* Place a buy stop-limit: trigger at 105, limit at 104 */
    reset_log();
    me_submit_stop(&me, ME_ORDER_STOP_LIMIT, OB_SIDE_BID,
                   OB_PRICE_FROM_DOUBLE(105.00),
                   OB_PRICE_FROM_DOUBLE(104.00), 30, 0, &r);
    me_run_events(&me);
    assert(r.status == OB_STATUS_OK);
    assert(me.stop_count == 1);

    /* Trigger it: trade at 105 */
    reset_log();
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_BID,
                    OB_PRICE_FROM_DOUBLE(105.00), 10, &r);
    me_run_events(&me);

    printf("  Trade at 105 triggers stop-limit\n");
    print_log();

    assert(me.stop_count == 0);
    assert(count_events(ME_EVENT_STOP_TRIGGERED) == 1);
    /* The stop-limit at 104 can't cross the 105 ask, so it should rest as a bid */
    assert(ob_best_bid(&me.book) == OB_PRICE_FROM_DOUBLE(104.00));

    PASS();
    me_destroy(&me);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 13 — Stop never triggers → stays dormant, cancel works
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_stop_cancel(void)
{
    SECTION("Test 13: Stop cancel before trigger");

    me_engine_t me;
    me_init(&me, "STOP3", log_handler, NULL);

    me_result_t r;

    /* Place a sell stop (triggers when price <= 90) */
    me_submit_stop(&me, ME_ORDER_STOP, OB_SIDE_ASK,
                   OB_PRICE_FROM_DOUBLE(90.00), 0, 100, 0, &r);
    me_run_events(&me);
    ob_order_id_t stop_id = r.id;
    assert(me.stop_count == 1);

    /* Cancel it before it triggers */
    reset_log();
    bool ok = me_cancel_stop(&me, stop_id);
    me_run_events(&me);

    printf("  Cancelled dormant stop #%llu\n", (unsigned long long)stop_id);
    print_log();

    assert(ok);
    assert(me.stop_count == 0);
    assert(count_events(ME_EVENT_ORDER_CANCELLED) == 1);

    PASS();
    me_destroy(&me);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 14 — Day order expires after TTL → auto-cancelled
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_day_order_expiry(void)
{
    SECTION("Test 14: Day order expiry");

    me_engine_t me;
    me_init(&me, "DAY1", log_handler, NULL);

    /* Submit a limit order with a very short TTL.
     * We simulate expiry by calling me_expire_orders with a future time.
     * The engine uses me_now() internally, so we just need the order
     * to have a non-zero expiry_ts and call expire with a larger time. */

    /* First, directly add an order with expiry to the book */
    ob_order_id_t id;
    ob_ts_t fake_expiry = 1000;  /* arbitrary small timestamp */
    ob_add_order(&me.book, OB_SIDE_BID,
                 OB_PRICE_FROM_DOUBLE(50.00), 100, fake_expiry,
                 &id, NULL, NULL);

    assert(ob_best_bid(&me.book) == OB_PRICE_FROM_DOUBLE(50.00));

    /* Sweep at time > expiry → should cancel the order */
    reset_log();
    uint32_t expired = me_expire_orders(&me, 2000);
    me_run_events(&me);

    printf("  Day order expired after sweep\n");
    print_log();

    assert(expired == 1);
    assert(ob_best_bid(&me.book) == 0);  /* book empty */
    assert(count_events(ME_EVENT_ORDER_CANCELLED) >= 1);

    PASS();
    me_destroy(&me);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 15 — Day order fills before TTL → normal fill
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_day_order_fill_before_expiry(void)
{
    SECTION("Test 15: Day order fills before expiry");

    me_engine_t me;
    me_init(&me, "DAY2", log_handler, NULL);

    me_result_t r;

    /* Place a resting ask with expiry far in the future */
    ob_order_id_t id;
    ob_ts_t far_future = (ob_ts_t)9999999999999ULL;
    ob_add_order(&me.book, OB_SIDE_ASK,
                 OB_PRICE_FROM_DOUBLE(200.00), 50, far_future,
                 &id, NULL, NULL);

    /* Aggressive bid crosses it — should fill normally */
    reset_log();
    me_submit_order(&me, ME_ORDER_LIMIT, OB_SIDE_BID,
                    OB_PRICE_FROM_DOUBLE(200.00), 50, &r);
    me_run_events(&me);

    printf("  Day ask filled by aggressive bid before expiry\n");
    print_log();

    assert(r.status == OB_STATUS_FILLED);
    assert(r.qty_filled == 50);
    assert(count_events(ME_EVENT_TRADE) == 1);

    /* Sweep should find nothing to expire */
    uint32_t expired = me_expire_orders(&me, far_future + 1);
    assert(expired == 0);

    PASS();
    me_destroy(&me);
}

/* ─── main ──────────────────────────────────────────────────────────────── */

int main(void)
{
    printf(CYN
           "╔════════════════════════════════════════════╗\n"
           "║     Matching Engine — Full Test Suite      ║\n"
           "╚════════════════════════════════════════════╝\n"
           RST);

    test_limit_resting();
    test_limit_full_cross();
    test_limit_partial_cross();
    test_market_sweep();
    test_ioc_partial();
    test_fok();
    test_price_time_priority();
    test_cancel_modify();
    test_validation();
    test_full_scenario();
    test_stop_market_trigger();
    test_stop_limit_trigger();
    test_stop_cancel();
    test_day_order_expiry();
    test_day_order_fill_before_expiry();

    printf(GRN
           "\n╔════════════════════════════════════════════╗\n"
           "║          All 15 tests passed  ✓           ║\n"
           "╚════════════════════════════════════════════╝\n"
           RST "\n");
    return 0;
}