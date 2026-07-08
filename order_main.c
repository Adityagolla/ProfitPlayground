/*
 * order_main.c — demo & smoke-tests for the Phase 2 order book
 *
 * Compile:
 *   gcc -Wall -Wextra -O2 -o ob_demo order_main.c orderbook.c && ./ob_demo
 */
#if defined(__MINGW32__) || defined(__MINGW64__)
#define __USE_MINGW_ANSI_STDIO 1
#endif

#include "orderbook.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

/* ─── Test 1: basic resting orders, no match ──────────────────────────── */
static void test_resting_orders(void)
{
    printf("\n=== TEST 1: resting limit orders (no match) ===\n");

    /* Heap-allocate: ob_book_t is ~400 KB with embedded pools */
    ob_book_t *book = (ob_book_t *)malloc(sizeof(ob_book_t));
    assert(book);
    ob_init(book, "AAPL");

    ob_order_id_t id;
    ob_status_t   st;

    /* Three bids at different prices */
    st = ob_add_order(book, OB_SIDE_BID, OB_PRICE_FROM_DOUBLE(149.50), 100, 0, &id, NULL, NULL);
    assert(st == OB_STATUS_OK);
    printf("  Bid #%llu resting @ 149.50 x 100\n", (unsigned long long)id);

    st = ob_add_order(book, OB_SIDE_BID, OB_PRICE_FROM_DOUBLE(149.00), 200, 0, &id, NULL, NULL);
    assert(st == OB_STATUS_OK);
    printf("  Bid #%llu resting @ 149.00 x 200\n", (unsigned long long)id);

    st = ob_add_order(book, OB_SIDE_BID, OB_PRICE_FROM_DOUBLE(148.75), 150, 0, &id, NULL, NULL);
    assert(st == OB_STATUS_OK);
    printf("  Bid #%llu resting @ 148.75 x 150\n", (unsigned long long)id);

    /* Two asks */
    st = ob_add_order(book, OB_SIDE_ASK, OB_PRICE_FROM_DOUBLE(150.00), 80, 0, &id, NULL, NULL);
    assert(st == OB_STATUS_OK);
    printf("  Ask #%llu resting @ 150.00 x 80\n",  (unsigned long long)id);

    st = ob_add_order(book, OB_SIDE_ASK, OB_PRICE_FROM_DOUBLE(150.50), 300, 0, &id, NULL, NULL);
    assert(st == OB_STATUS_OK);
    printf("  Ask #%llu resting @ 150.50 x 300\n", (unsigned long long)id);

    printf("  best bid=%.4f  best ask=%.4f  spread=%.4f\n",
           OB_PRICE_TO_DOUBLE(ob_best_bid(book)),
           OB_PRICE_TO_DOUBLE(ob_best_ask(book)),
           OB_PRICE_TO_DOUBLE(ob_spread(book)));

    ob_print(book, 0);
    ob_print_stats(book);
    ob_destroy(book);
    free(book);
    printf("  PASS\n");
}

/* ─── Test 2: cancel order ──────────────────────────────────────────────── */
static void test_cancel(void)
{
    printf("\n=== TEST 2: cancel order ===\n");

    ob_book_t *book = (ob_book_t *)malloc(sizeof(ob_book_t));
    assert(book);
    ob_init(book, "NVDA");

    ob_order_id_t id1, id2;
    ob_status_t   st;

    ob_add_order(book, OB_SIDE_BID, OB_PRICE_FROM_DOUBLE(400.00), 200, 0, &id1, NULL, NULL);
    ob_add_order(book, OB_SIDE_BID, OB_PRICE_FROM_DOUBLE(399.50), 100, 0, &id2, NULL, NULL);
    printf("  Two bids: #%llu @ 400.00, #%llu @ 399.50\n",
           (unsigned long long)id1, (unsigned long long)id2);

    st = ob_cancel_order(book, id1);
    assert(st == OB_STATUS_OK);
    printf("  Cancelled #%llu\n", (unsigned long long)id1);

    assert(ob_best_bid(book) == OB_PRICE_FROM_DOUBLE(399.50));

    st = ob_cancel_order(book, id1);   /* double-cancel → not found */
    assert(st == OB_STATUS_NOT_FOUND);
    printf("  Double-cancel correctly returned NOT_FOUND\n");

    ob_destroy(book);
    free(book);
    printf("  PASS\n");
}

/* ─── Test 3: modify order ──────────────────────────────────────────────── */
static void test_modify(void)
{
    printf("\n=== TEST 3: modify order ===\n");

    ob_book_t *book = (ob_book_t *)malloc(sizeof(ob_book_t));
    assert(book);
    ob_init(book, "AMZN");

    ob_order_id_t id;

    ob_add_order(book, OB_SIDE_BID, OB_PRICE_FROM_DOUBLE(180.00), 500, 0, &id, NULL, NULL);
    printf("  Bid #%llu: buy 500 @ 180.00\n", (unsigned long long)id);

    /* Modify price upward (loses time priority) */
    ob_status_t st = ob_modify_order(book, id,
                                     OB_PRICE_FROM_DOUBLE(181.00), 500);
    assert(st == OB_STATUS_OK);
    printf("  Modified #%llu -> 500 @ 181.00\n", (unsigned long long)id);
    assert(ob_best_bid(book) == OB_PRICE_FROM_DOUBLE(181.00));

    /* Modify qty downward */
    st = ob_modify_order(book, id,
                         OB_PRICE_FROM_DOUBLE(181.00), 250);
    assert(st == OB_STATUS_OK);
    printf("  Modified #%llu -> 250 @ 181.00\n", (unsigned long long)id);

    ob_order_t snap;
    assert(ob_get_order(book, id, &snap));
    assert(snap.qty_remain == 250);
    printf("  Snapshot: qty_remain=%llu  CORRECT\n",
           (unsigned long long)snap.qty_remain);

    ob_destroy(book);
    free(book);
    printf("  PASS\n");
}

/* ─── Test 4: FIFO within same price level ─────────────────────────────── */
static void test_fifo_within_level(void)
{
    printf("\n=== TEST 4: FIFO priority within same price level ===\n");

    ob_book_t *book = (ob_book_t *)malloc(sizeof(ob_book_t));
    assert(book);
    ob_init(book, "META");

    ob_order_id_t id_a, id_b;

    /* Two asks at the same price */
    ob_add_order(book, OB_SIDE_ASK, OB_PRICE_FROM_DOUBLE(500.00), 30, 0, &id_a, NULL, NULL);
    ob_add_order(book, OB_SIDE_ASK, OB_PRICE_FROM_DOUBLE(500.00), 50, 0, &id_b, NULL, NULL);
    printf("  Ask #%llu (first) and #%llu (second) both @ 500.00\n",
           (unsigned long long)id_a, (unsigned long long)id_b);

    /* Verify FIFO: first order should be at head of level queue */
    ob_order_t snap_a, snap_b;
    assert(ob_get_order(book, id_a, &snap_a));
    assert(ob_get_order(book, id_b, &snap_b));
    /* id_a submitted first, so it has the earlier timestamp */
    assert(snap_a.ts_submit <= snap_b.ts_submit);
    printf("  Order #%llu has earlier timestamp — FIFO correct\n",
           (unsigned long long)id_a);

    ob_print(book, 0);
    ob_destroy(book);
    free(book);
    printf("  PASS\n");
}

/* ─── Test 5: pool stats ───────────────────────────────────────────────── */
static void test_pool_stats(void)
{
    printf("\n=== TEST 5: pool utilisation ===\n");

    ob_book_t *book = (ob_book_t *)malloc(sizeof(ob_book_t));
    assert(book);
    ob_init(book, "POOL");

    ob_order_id_t id;
    for (int i = 0; i < 100; ++i)
        ob_add_order(book, OB_SIDE_BID, OB_PRICE_FROM_DOUBLE(100.00 + i * 0.01), 10, 0, &id, NULL, NULL);

    printf("  Added 100 orders across 100 levels\n");
    ob_print_stats(book);

    /* Cancel all */
    for (uint64_t i = 1; i <= 100; ++i)
        ob_cancel_order(book, i);

    printf("  Cancelled all 100 orders\n");
    ob_print_stats(book);

    ob_destroy(book);
    free(book);
    printf("  PASS\n");
}

/* ─── main ──────────────────────────────────────────────────────────────── */
int main(void)
{
    printf("+==========================================+\n");
    printf("|  Order Book Phase 2 — Smoke Test Suite   |\n");
    printf("|  Pools + Cache-friendly + Threading-ready|\n");
    printf("+==========================================+\n");

    printf("\n  sizeof(ob_book_t)  = %zu bytes\n", sizeof(ob_book_t));
    printf("  sizeof(ob_order_t) = %zu bytes\n", sizeof(ob_order_t));
    printf("  sizeof(ob_level_t) = %zu bytes\n\n", sizeof(ob_level_t));

    test_resting_orders();
    test_cancel();
    test_modify();
    test_fifo_within_level();
    test_pool_stats();

    printf("\n  All tests passed.\n\n");
    return 0;
}