/*
 * orderbook.c — Limit order book (Phase 2: pools + cache-friendly)
 *
 * Enable proper C99 printf format specifiers on MinGW
 */
#if defined(__MINGW32__) || defined(__MINGW64__)
#define __USE_MINGW_ANSI_STDIO 1
#endif

/*
 * Design contract
 * ---------------
 *  - Preallocated memory pools: zero malloc/free on hot path.
 *  - Cache-friendly: structs use uint16_t pool indices, not pointers.
 *  - All state lives in ob_book_t (no globals except lookup table).
 *  - Optional threading via -DOB_ENABLE_THREADS compile flag.
 *  - No matching engine: orders rest on the book.
 */

#include "orderbook.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <limits.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * TIMESTAMP — monotonic nanosecond clock
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#include <windows.h>
static ob_ts_t now_ns(void)
{
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (ob_ts_t)(count.QuadPart * 1000000000ULL / freq.QuadPart);
}
#else
#include <time.h>
static ob_ts_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ob_ts_t)ts.tv_sec * 1000000000ULL + (ob_ts_t)ts.tv_nsec;
}
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * ▓▓▓ PHASE 2 ▓▓▓ — Pool allocators (zero malloc on hot path)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ALLOCATE ORDER — pop a free slot from the order pool free-stack */
static uint16_t order_pool_alloc(ob_book_t *book)
{
    if (book->order_free_top == 0) return OB_NULL_IDX;
    return book->order_free_stack[--book->order_free_top];
}

/* RETURN ORDER TO POOL — push slot index back onto free-stack.
 * Clears id/expiry so raw pool sweeps (me_expire_orders) can rely on
 * "id == 0 means empty slot" and never resurrect a freed order. */
static void order_pool_free(ob_book_t *book, uint16_t idx)
{
    book->order_pool[idx].id        = 0;
    book->order_pool[idx].expiry_ts = 0;
    book->order_free_stack[book->order_free_top++] = idx;
}

/* ALLOCATE LEVEL — pop a free slot from the level pool free-stack */
static uint16_t level_pool_alloc(ob_book_t *book)
{
    if (book->level_free_top == 0) return OB_NULL_IDX;
    return book->level_free_stack[--book->level_free_top];
}

/* RETURN LEVEL TO POOL — push slot index back onto free-stack */
static void level_pool_free(ob_book_t *book, uint16_t idx)
{
    book->level_free_stack[book->level_free_top++] = idx;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ORDER ID LOOKUP — hash table mapping order id → pool index
 *
 * Open-addressed hash table with Fibonacci hashing.
 * Stores pool indices (uint16_t) instead of pointers for cache locality.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define LOOKUP_CAP  (OB_MAX_ORDERS * 2)   /* load factor ≤ 0.5              */

typedef struct {
    ob_order_id_t key;      /* 0 = empty slot                               */
    uint16_t      pool_idx; /* index into book->order_pool[]                */
} lookup_entry_t;

typedef struct {
    lookup_entry_t slots[LOOKUP_CAP];
} lookup_table_t;

static lookup_table_t *g_lookup = NULL;

static void lookup_init(void)
{
    if (!g_lookup) {
        g_lookup = (lookup_table_t *)calloc(1, sizeof(lookup_table_t));
    } else {
        memset(g_lookup, 0, sizeof(lookup_table_t));
    }
}

static void lookup_destroy(void)
{
    free(g_lookup);
    g_lookup = NULL;
}

static uint32_t lookup_slot(ob_order_id_t id)
{
    /* Fibonacci hashing */
    return (uint32_t)((id * 11400714819323198485ULL) >> 32) % LOOKUP_CAP;
}

static void lookup_insert(ob_order_id_t id, uint16_t pool_idx)
{
    uint32_t s = lookup_slot(id);
    while (g_lookup->slots[s].key != 0) {
        s = (s + 1) % LOOKUP_CAP;
    }
    g_lookup->slots[s].key      = id;
    g_lookup->slots[s].pool_idx = pool_idx;
}

/* FIND ORDER BY ID — returns pool index, or OB_NULL_IDX if not found */
static uint16_t lookup_find(ob_order_id_t id)
{
    uint32_t s = lookup_slot(id);
    for (uint32_t i = 0; i < LOOKUP_CAP; ++i) {
        uint32_t idx = (s + i) % LOOKUP_CAP;
        if (g_lookup->slots[idx].key == 0)  return OB_NULL_IDX;
        if (g_lookup->slots[idx].key == id) return g_lookup->slots[idx].pool_idx;
    }
    return OB_NULL_IDX;
}

static void lookup_remove(ob_order_id_t id)
{
    uint32_t s = lookup_slot(id);
    for (uint32_t i = 0; i < LOOKUP_CAP; ++i) {
        uint32_t idx = (s + i) % LOOKUP_CAP;
        if (g_lookup->slots[idx].key == 0)  return;
        if (g_lookup->slots[idx].key == id) {
            /* Tombstone-free removal: shift subsequent cluster back */
            g_lookup->slots[idx].key = 0;
            uint32_t j = (idx + 1) % LOOKUP_CAP;
            while (g_lookup->slots[j].key != 0) {
                lookup_entry_t e = g_lookup->slots[j];
                g_lookup->slots[j].key = 0;
                lookup_insert(e.key, e.pool_idx);
                j = (j + 1) % LOOKUP_CAP;
            }
            return;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * AVL TREE — price-level index sorted by price (pool-index based)
 *
 * All node references are uint16_t indices into book->level_pool[].
 * OB_NULL_IDX serves as NULL. This keeps the tree cache-friendly.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define LV(book, i) (&(book)->level_pool[(i)])

static int avl_height(ob_book_t *b, uint16_t i)
{
    return (i == OB_NULL_IDX) ? 0 : LV(b, i)->height;
}

static int avl_balance(ob_book_t *b, uint16_t i)
{
    if (i == OB_NULL_IDX) return 0;
    return avl_height(b, LV(b, i)->left_idx) - avl_height(b, LV(b, i)->right_idx);
}

static void avl_update_height(ob_book_t *b, uint16_t i)
{
    if (i == OB_NULL_IDX) return;
    int lh = avl_height(b, LV(b, i)->left_idx);
    int rh = avl_height(b, LV(b, i)->right_idx);
    LV(b, i)->height = 1 + (lh > rh ? lh : rh);
}

static uint16_t avl_rotate_right(ob_book_t *b, uint16_t yi)
{
    uint16_t xi  = LV(b, yi)->left_idx;
    uint16_t t2  = LV(b, xi)->right_idx;
    LV(b, xi)->right_idx = yi;
    LV(b, yi)->left_idx  = t2;
    avl_update_height(b, yi);
    avl_update_height(b, xi);
    return xi;
}

static uint16_t avl_rotate_left(ob_book_t *b, uint16_t xi)
{
    uint16_t yi  = LV(b, xi)->right_idx;
    uint16_t t2  = LV(b, yi)->left_idx;
    LV(b, yi)->left_idx  = xi;
    LV(b, xi)->right_idx = t2;
    avl_update_height(b, xi);
    avl_update_height(b, yi);
    return yi;
}

/* INSERT LEVEL INTO AVL — returns new root index */
static uint16_t avl_insert(ob_book_t *b, uint16_t root, uint16_t node)
{
    if (root == OB_NULL_IDX) return node;
    ob_price_t np = LV(b, node)->price;
    ob_price_t rp = LV(b, root)->price;

    if (np < rp)
        LV(b, root)->left_idx  = avl_insert(b, LV(b, root)->left_idx,  node);
    else if (np > rp)
        LV(b, root)->right_idx = avl_insert(b, LV(b, root)->right_idx, node);
    else
        return root; /* duplicate price — caller merges into existing level */

    avl_update_height(b, root);
    int bal = avl_balance(b, root);

    if (bal >  1 && np < LV(b, LV(b, root)->left_idx)->price)
        return avl_rotate_right(b, root);
    if (bal < -1 && np > LV(b, LV(b, root)->right_idx)->price)
        return avl_rotate_left(b, root);
    if (bal >  1 && np > LV(b, LV(b, root)->left_idx)->price) {
        LV(b, root)->left_idx = avl_rotate_left(b, LV(b, root)->left_idx);
        return avl_rotate_right(b, root);
    }
    if (bal < -1 && np < LV(b, LV(b, root)->right_idx)->price) {
        LV(b, root)->right_idx = avl_rotate_right(b, LV(b, root)->right_idx);
        return avl_rotate_left(b, root);
    }
    return root;
}

/* FIND MIN/MAX — walk to leftmost/rightmost node */
static uint16_t avl_min(ob_book_t *b, uint16_t i)
{
    while (i != OB_NULL_IDX && LV(b, i)->left_idx != OB_NULL_IDX)
        i = LV(b, i)->left_idx;
    return i;
}

static uint16_t avl_max(ob_book_t *b, uint16_t i)
{
    while (i != OB_NULL_IDX && LV(b, i)->right_idx != OB_NULL_IDX)
        i = LV(b, i)->right_idx;
    return i;
}

/* REMOVE LEVEL FROM AVL — returns new root index, frees the level slot */
static uint16_t avl_remove(ob_book_t *b, uint16_t root, ob_price_t price)
{
    if (root == OB_NULL_IDX) return OB_NULL_IDX;

    if (price < LV(b, root)->price)
        LV(b, root)->left_idx  = avl_remove(b, LV(b, root)->left_idx,  price);
    else if (price > LV(b, root)->price)
        LV(b, root)->right_idx = avl_remove(b, LV(b, root)->right_idx, price);
    else {
        /* ─── LEVEL DELETE: node found ─── */
        if (LV(b, root)->left_idx == OB_NULL_IDX ||
            LV(b, root)->right_idx == OB_NULL_IDX) {
            uint16_t tmp = (LV(b, root)->left_idx != OB_NULL_IDX)
                         ?  LV(b, root)->left_idx : LV(b, root)->right_idx;
            level_pool_free(b, root);
            return tmp;
        }
        uint16_t succ = avl_min(b, LV(b, root)->right_idx);
        LV(b, root)->price       = LV(b, succ)->price;
        LV(b, root)->total_qty   = LV(b, succ)->total_qty;
        LV(b, root)->order_count = LV(b, succ)->order_count;
        LV(b, root)->head_idx    = LV(b, succ)->head_idx;
        LV(b, root)->tail_idx    = LV(b, succ)->tail_idx;
        LV(b, root)->right_idx   = avl_remove(b, LV(b, root)->right_idx, LV(b, succ)->price);
    }
    avl_update_height(b, root);
    int bal = avl_balance(b, root);
    if (bal >  1 && avl_balance(b, LV(b, root)->left_idx)  >= 0) return avl_rotate_right(b, root);
    if (bal >  1 && avl_balance(b, LV(b, root)->left_idx)  <  0) { LV(b, root)->left_idx  = avl_rotate_left(b, LV(b, root)->left_idx);  return avl_rotate_right(b, root); }
    if (bal < -1 && avl_balance(b, LV(b, root)->right_idx) <= 0) return avl_rotate_left(b, root);
    if (bal < -1 && avl_balance(b, LV(b, root)->right_idx) >  0) { LV(b, root)->right_idx = avl_rotate_right(b, LV(b, root)->right_idx); return avl_rotate_left(b, root); }
    return root;
}

/* FIND LEVEL BY PRICE — returns level pool index or OB_NULL_IDX */
static uint16_t avl_find(ob_book_t *b, uint16_t root, ob_price_t price)
{
    while (root != OB_NULL_IDX) {
        if      (price < LV(b, root)->price) root = LV(b, root)->left_idx;
        else if (price > LV(b, root)->price) root = LV(b, root)->right_idx;
        else                                 return root;
    }
    return OB_NULL_IDX;
}

/* RESET ALL — walk tree and return all slots to pools (used by ob_destroy) */
static void avl_free_all(ob_book_t *b, uint16_t root)
{
    if (root == OB_NULL_IDX) return;
    avl_free_all(b, LV(b, root)->left_idx);
    avl_free_all(b, LV(b, root)->right_idx);
    /* Return all orders in this level's FIFO queue to pool */
    uint16_t oi = LV(b, root)->head_idx;
    while (oi != OB_NULL_IDX) {
        uint16_t nxt = b->order_pool[oi].next_idx;
        order_pool_free(b, oi);
        oi = nxt;
    }
    level_pool_free(b, root);
}

/* ─── LEVEL HELPERS — FIFO queue at a single price point ────────────────── */

/* CREATE LEVEL — allocate from pool, initialise for given price */
static uint16_t level_new(ob_book_t *b, ob_price_t price)
{
    uint16_t li = level_pool_alloc(b);
    if (li == OB_NULL_IDX) return OB_NULL_IDX;
    ob_level_t *lv = LV(b, li);
    memset(lv, 0, sizeof(*lv));
    lv->price    = price;
    lv->height   = 1;
    lv->head_idx = OB_NULL_IDX;
    lv->tail_idx = OB_NULL_IDX;
    lv->left_idx = OB_NULL_IDX;
    lv->right_idx= OB_NULL_IDX;
    return li;
}

/* ENQUEUE ORDER — append to tail of level's FIFO (time priority preserved) */
static void level_enqueue(ob_book_t *b, uint16_t li, uint16_t oi)
{
    ob_level_t *lv = LV(b, li);
    ob_order_t *o  = &b->order_pool[oi];
    o->prev_idx = lv->tail_idx;
    o->next_idx = OB_NULL_IDX;
    if (lv->tail_idx != OB_NULL_IDX) b->order_pool[lv->tail_idx].next_idx = oi;
    else                             lv->head_idx = oi;
    lv->tail_idx = oi;
    lv->total_qty   += o->qty_remain;
    lv->order_count++;
}

/* DEQUEUE ORDER — remove arbitrary order from level's FIFO queue */
static void level_dequeue(ob_book_t *b, uint16_t li, uint16_t oi)
{
    ob_level_t *lv = LV(b, li);
    ob_order_t *o  = &b->order_pool[oi];
    if (o->prev_idx != OB_NULL_IDX) b->order_pool[o->prev_idx].next_idx = o->next_idx;
    else                            lv->head_idx = o->next_idx;
    if (o->next_idx != OB_NULL_IDX) b->order_pool[o->next_idx].prev_idx = o->prev_idx;
    else                            lv->tail_idx = o->prev_idx;
    o->prev_idx = o->next_idx = OB_NULL_IDX;
    lv->total_qty   -= o->qty_remain;
    lv->order_count--;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * THREADING MACROS — compile with -DOB_ENABLE_THREADS to activate
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifdef OB_ENABLE_THREADS

static void rwlock_read_lock(ob_rwlock_t *lk) {
    for (;;) {
        int32_t s = atomic_load_explicit(&lk->state, memory_order_relaxed);
        if (s >= 0 && atomic_compare_exchange_weak_explicit(
                &lk->state, &s, s + 1,
                memory_order_acquire, memory_order_relaxed))
            return;
    }
}
static void rwlock_read_unlock(ob_rwlock_t *lk) {
    atomic_fetch_sub_explicit(&lk->state, 1, memory_order_release);
}
static void rwlock_write_lock(ob_rwlock_t *lk) {
    int32_t expected = 0;
    while (!atomic_compare_exchange_weak_explicit(
            &lk->state, &expected, -1,
            memory_order_acquire, memory_order_relaxed))
        expected = 0;
}
static void rwlock_write_unlock(ob_rwlock_t *lk) {
    atomic_store_explicit(&lk->state, 0, memory_order_release);
}

#define OB_READ_LOCK(b)    rwlock_read_lock(&(b)->lock)
#define OB_READ_UNLOCK(b)  rwlock_read_unlock(&(b)->lock)
#define OB_WRITE_LOCK(b)   rwlock_write_lock(&(b)->lock)
#define OB_WRITE_UNLOCK(b) rwlock_write_unlock(&(b)->lock)
#else
#define OB_READ_LOCK(b)    ((void)0)
#define OB_READ_UNLOCK(b)  ((void)0)
#define OB_WRITE_LOCK(b)   ((void)0)
#define OB_WRITE_UNLOCK(b) ((void)0)
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API implementation
 * ═══════════════════════════════════════════════════════════════════════════ */

/* INITIALISE BOOK — populate free stacks for both pools */
void ob_init(ob_book_t *book, const char *symbol)
{
    memset(book, 0, sizeof(*book));
    strncpy(book->symbol, symbol, OB_MAX_SYMBOL_LEN - 1);
    book->sides[OB_SIDE_BID].side     = OB_SIDE_BID;
    book->sides[OB_SIDE_BID].root_idx = OB_NULL_IDX;
    book->sides[OB_SIDE_ASK].side     = OB_SIDE_ASK;
    book->sides[OB_SIDE_ASK].root_idx = OB_NULL_IDX;
    book->next_order_id = 1;  /* 0 is reserved as "invalid" */

    /* Phase 2: fill order pool free-stack */
    for (uint16_t i = 0; i < OB_MAX_ORDERS; ++i)
        book->order_free_stack[i] = i;
    book->order_free_top = OB_MAX_ORDERS;

    /* Phase 2: fill level pool free-stack */
    uint16_t level_cap = OB_MAX_LEVELS * OB_SIDE_COUNT;
    for (uint16_t i = 0; i < level_cap; ++i)
        book->level_free_stack[i] = i;
    book->level_free_top = level_cap;

    lookup_init();
#ifdef OB_ENABLE_THREADS
    atomic_init(&book->lock.state, 0);
#endif
}

/* DESTROY BOOK — return all pool slots, free lookup table */
void ob_destroy(ob_book_t *book)
{
    avl_free_all(book, book->sides[OB_SIDE_BID].root_idx);
    avl_free_all(book, book->sides[OB_SIDE_ASK].root_idx);
    book->sides[OB_SIDE_BID].root_idx = OB_NULL_IDX;
    book->sides[OB_SIDE_ASK].root_idx = OB_NULL_IDX;
    lookup_destroy();
}

/* ─── MATCHING ENGINE — Price-Time priority fill loop ────────────────────────
 *
 * Called by ob_add_order() when the incoming order can cross the opposite side.
 *
 * How it works:
 *   1. Identify the best level on the opposite side (avl_min for asks, avl_max
 *      for bids) — this is the cheapest ask (or highest bid) available.
 *   2. For each level that is still within the aggressor's price limit:
 *        a. Walk the level's FIFO queue from head to tail.
 *        b. For each resting order: fill min(qty_remain_aggressor,
 *           qty_remain_resting), write an ob_match_t record, update both
 *           order qty_remain fields.
 *        c. If a resting order is fully filled: remove it from the FIFO,
 *           return its pool slot, update level stats.
 *        d. If the level becomes empty: remove it from the AVL tree.
 *   3. Stop when aggressor is fully filled or no more crossing levels.
 *
 * This is all index-based (uint16_t) — no pointer chasing, no malloc.
 */
static void match_against_side(ob_book_t   *book,
                                ob_side_t    opp_side,
                                ob_price_t   aggressor_price,
                                ob_qty_t    *qty_remain,       /* in/out      */
                                ob_match_t  *scratch,          /* fill buffer */
                                uint32_t    *mc)               /* fill count  */
{
    ob_side_t_s *opp = &book->sides[opp_side];

    while (*qty_remain > 0 && opp->root_idx != OB_NULL_IDX) {

        /* ─── Locate best price level on opposite side ─── */
        uint16_t best_li = (opp_side == OB_SIDE_ASK)
                           ? avl_min(book, opp->root_idx)   /* lowest ask  */
                           : avl_max(book, opp->root_idx);  /* highest bid */

        if (best_li == OB_NULL_IDX) break;
        ob_level_t *lv = LV(book, best_li);

        /* ─── Price boundary check: stop if the level no longer crosses ─── */
        if (opp_side == OB_SIDE_ASK && lv->price > aggressor_price) break;
        if (opp_side == OB_SIDE_BID && lv->price < aggressor_price) break;

        /* ─── Walk this level's FIFO queue, filling head-first (time priority) ─── */
        while (*qty_remain > 0 && lv->head_idx != OB_NULL_IDX) {
            uint16_t    rest_oi = lv->head_idx;
            ob_order_t *rest    = &book->order_pool[rest_oi];

            /* Fill quantity = min(aggressor needs, resting has) */
            ob_qty_t fill_qty = (*qty_remain < rest->qty_remain)
                                ? *qty_remain : rest->qty_remain;

            /* ─── Record the fill into scratch buffer ─── */
            if (scratch && *mc < OB_MAX_ORDERS) {
                scratch[*mc] = (ob_match_t){
                    .resting_id = rest->id,
                    .price      = lv->price,   /* execution at resting price */
                    .qty        = fill_qty,
                    .ts         = now_ns(),
                };
                (*mc)++;
            }

            /* ─── Update aggressor remaining qty ─── */
            *qty_remain      -= fill_qty;

            /* ─── Update resting order qty; dequeue if fully filled ─── */
            rest->qty_remain -= fill_qty;
            lv->total_qty    -= fill_qty;
            opp->total_qty   -= fill_qty;

            if (rest->qty_remain == 0) {
                /* Resting order fully filled: remove from FIFO and lookup */
                level_dequeue(book, best_li, rest_oi);
                lookup_remove(rest->id);
                order_pool_free(book, rest_oi);
            }
        }

        /* ─── If level is now empty, remove from AVL tree ─── */
        if (lv->order_count == 0) {
            opp->root_idx = avl_remove(book, opp->root_idx, lv->price);
            opp->level_count--;
        }
    }
}

/* STORE / MATCH ORDER — match against opposite side, rest any remainder */
ob_status_t ob_add_order(ob_book_t     *book,
                         ob_side_t      side,
                         ob_price_t     price,
                         ob_qty_t       qty,
                         ob_ts_t        expiry_ts,
                         ob_order_id_t *out_id,
                         ob_match_t    *match_scratch,
                         uint32_t      *match_count)
{
    OB_WRITE_LOCK(book);

    *out_id = 0;
    if (match_count) *match_count = 0;

    if (price <= 0 || qty == 0 || side >= OB_SIDE_COUNT) {
        OB_WRITE_UNLOCK(book);
        return OB_STATUS_REJECTED;
    }

    /* ─── Allocate an order slot for the aggressor from the preallocated pool ─── */
    uint16_t oi = order_pool_alloc(book);
    if (oi == OB_NULL_IDX) { OB_WRITE_UNLOCK(book); return OB_STATUS_REJECTED; }

    ob_order_t *o   = &book->order_pool[oi];
    ob_ts_t     now = now_ns();
    *o = (ob_order_t){
        .id         = book->next_order_id++,
        .price      = price,
        .qty_remain = qty,
        .side       = side,
        .prev_idx   = OB_NULL_IDX,
        .next_idx   = OB_NULL_IDX,
        .qty        = qty,
        .ts_submit  = now,
        .ts_update  = now,
        .expiry_ts  = expiry_ts,
    };
    *out_id = o->id;
    book->total_orders_added++;

    /* ─── MATCHING: walk opposite side and fill crossing levels ─── */
    ob_side_t opp_side = (side == OB_SIDE_BID) ? OB_SIDE_ASK : OB_SIDE_BID;
    uint32_t  mc_local = 0;
    match_against_side(book, opp_side, price, &o->qty_remain,
                       match_scratch, &mc_local);
    if (match_count) *match_count = mc_local;

    /* ─── If fully filled, return the pool slot — order never rests ─── */
    if (o->qty_remain == 0) {
        order_pool_free(book, oi);
        OB_WRITE_UNLOCK(book);
        return OB_STATUS_FILLED;
    }

    /* ─── Partially or unfilled: rest remainder in our own side ─── */
    ob_side_t_s *our_side = &book->sides[side];
    uint16_t li = avl_find(book, our_side->root_idx, price);

    if (li == OB_NULL_IDX) {
        /* ─── LEVEL CREATE: open a new price level for this order ─── */
        if (our_side->level_count >= OB_MAX_LEVELS) {
            order_pool_free(book, oi);
            OB_WRITE_UNLOCK(book);
            return OB_STATUS_REJECTED;
        }
        li = level_new(book, price);
        if (li == OB_NULL_IDX) {
            order_pool_free(book, oi);
            OB_WRITE_UNLOCK(book);
            return OB_STATUS_REJECTED;
        }
        our_side->root_idx = avl_insert(book, our_side->root_idx, li);
        our_side->level_count++;
    }

    /* ─── INSERT: order enters the book at the tail of this level (FIFO) ─── */
    level_enqueue(book, li, oi);
    our_side->total_qty += o->qty_remain;
    lookup_insert(o->id, oi);

    OB_WRITE_UNLOCK(book);
    /* Return PARTIAL if we got any fills, OK if none */
    return (mc_local > 0) ? OB_STATUS_PARTIAL : OB_STATUS_OK;
}


/* REMOVE ORDER — detach from level FIFO, return slot to pool */
ob_status_t ob_cancel_order(ob_book_t *book, ob_order_id_t id)
{
    OB_WRITE_LOCK(book);

    /* ─── REMOVE: locate order by id in lookup table ─── */
    uint16_t oi = lookup_find(id);
    if (oi == OB_NULL_IDX) { OB_WRITE_UNLOCK(book); return OB_STATUS_NOT_FOUND; }

    ob_order_t *o = &book->order_pool[oi];
    ob_side_t_s *side = &book->sides[o->side];
    uint16_t li = avl_find(book, side->root_idx, o->price);
    if (li == OB_NULL_IDX) { OB_WRITE_UNLOCK(book); return OB_STATUS_NOT_FOUND; }

    side->total_qty -= o->qty_remain;

    /* ─── REMOVE: order leaves book (detach from FIFO queue) ─── */
    level_dequeue(book, li, oi);

    if (LV(book, li)->order_count == 0) {
        /* ─── LEVEL DELETE: empty price level removed from AVL tree ─── */
        side->root_idx = avl_remove(book, side->root_idx, LV(book, li)->price);
        side->level_count--;
    }

    lookup_remove(id);
    /* ─── REMOVE: order slot returned to pool ─── */
    order_pool_free(book, oi);
    book->total_orders_cancelled++;

    OB_WRITE_UNLOCK(book);
    return OB_STATUS_OK;
}

/* MODIFY ORDER — remove from old level, update fields, re-insert at new level */
ob_status_t ob_modify_order(ob_book_t    *book,
                            ob_order_id_t id,
                            ob_price_t    new_price,
                            ob_qty_t      new_qty)
{
    OB_WRITE_LOCK(book);

    if (new_price <= 0 || new_qty == 0) { OB_WRITE_UNLOCK(book); return OB_STATUS_REJECTED; }

    uint16_t oi = lookup_find(id);
    if (oi == OB_NULL_IDX) { OB_WRITE_UNLOCK(book); return OB_STATUS_NOT_FOUND; }

    ob_order_t *o = &book->order_pool[oi];
    bool price_changed = (new_price != o->price);
    bool qty_reduced   = (new_qty < o->qty_remain) && !price_changed;

    ob_side_t_s *side = &book->sides[o->side];
    uint16_t li = avl_find(book, side->root_idx, o->price);

    /* ─── REMOVE: detach order from current level ─── */
    side->total_qty -= o->qty_remain;
    level_dequeue(book, li, oi);
    if (LV(book, li)->order_count == 0) {
        /* ─── LEVEL DELETE: old level now empty ─── */
        side->root_idx = avl_remove(book, side->root_idx, LV(book, li)->price);
        side->level_count--;
    }

    /* Apply modifications */
    o->price      = new_price;
    o->qty_remain = new_qty;
    o->ts_update  = now_ns();
    (void)qty_reduced;  /* both paths re-enqueue at tail currently */

    /* ─── INSERT: find or create destination level ─── */
    uint16_t dst = avl_find(book, side->root_idx, new_price);
    if (dst == OB_NULL_IDX) {
        if (side->level_count >= OB_MAX_LEVELS) {
            lookup_remove(id);
            order_pool_free(book, oi);
            OB_WRITE_UNLOCK(book);
            return OB_STATUS_REJECTED;
        }
        /* ─── LEVEL CREATE: new destination level ─── */
        dst = level_new(book, new_price);
        side->root_idx = avl_insert(book, side->root_idx, dst);
        side->level_count++;
    }

    /* ─── INSERT: order re-enters book at new level (FIFO tail) ─── */
    level_enqueue(book, dst, oi);
    side->total_qty += o->qty_remain;

    OB_WRITE_UNLOCK(book);
    return OB_STATUS_OK;
}

/* ─── QUERIES — read-only access to book state ──────────────────────────── */

/* QUERY BEST BID — walk AVL tree to rightmost (highest price) node */
ob_price_t ob_best_bid(const ob_book_t *book)
{
    OB_READ_LOCK((ob_book_t*)book);
    uint16_t li = avl_max((ob_book_t*)book, book->sides[OB_SIDE_BID].root_idx);
    ob_price_t p = (li != OB_NULL_IDX) ? LV((ob_book_t*)book, li)->price : 0;
    OB_READ_UNLOCK((ob_book_t*)book);
    return p;
}

/* QUERY BEST ASK — walk AVL tree to leftmost (lowest price) node */
ob_price_t ob_best_ask(const ob_book_t *book)
{
    OB_READ_LOCK((ob_book_t*)book);
    uint16_t li = avl_min((ob_book_t*)book, book->sides[OB_SIDE_ASK].root_idx);
    ob_price_t p = (li != OB_NULL_IDX) ? LV((ob_book_t*)book, li)->price : 0;
    OB_READ_UNLOCK((ob_book_t*)book);
    return p;
}

ob_price_t ob_spread(const ob_book_t *book)
{
    ob_price_t bid = ob_best_bid(book);
    ob_price_t ask = ob_best_ask(book);
    if (!bid || !ask) return -1;
    return ask - bid;
}

ob_price_t ob_mid_price(const ob_book_t *book)
{
    ob_price_t bid = ob_best_bid(book);
    ob_price_t ask = ob_best_ask(book);
    if (!bid || !ask) return 0;
    return (bid + ask) / 2;
}

/* DEPTH QUERY — sum resting qty within price range of best */
static ob_qty_t depth_in_tree(ob_book_t *b, uint16_t root,
                              ob_side_t side, ob_price_t limit, ob_price_t range)
{
    if (root == OB_NULL_IDX) return 0;
    ob_qty_t qty = 0;
    ob_price_t p = LV(b, root)->price;
    if (side == OB_SIDE_BID) {
        if (p >= limit - range) qty += LV(b, root)->total_qty;
        qty += depth_in_tree(b, LV(b, root)->left_idx, side, limit, range);
        if (p >= limit - range)
            qty += depth_in_tree(b, LV(b, root)->right_idx, side, limit, range);
    } else {
        if (p <= limit + range) qty += LV(b, root)->total_qty;
        qty += depth_in_tree(b, LV(b, root)->right_idx, side, limit, range);
        if (p <= limit + range)
            qty += depth_in_tree(b, LV(b, root)->left_idx, side, limit, range);
    }
    return qty;
}

ob_qty_t ob_depth(const ob_book_t *book, ob_side_t side, ob_price_t price_range)
{
    if (price_range == (ob_price_t)UINT64_MAX)
        return book->sides[side].total_qty;
    ob_price_t best = (side == OB_SIDE_BID) ? ob_best_bid(book) : ob_best_ask(book);
    if (!best) return 0;
    return depth_in_tree((ob_book_t*)book, book->sides[side].root_idx, side, best, price_range);
}

/* GET ORDER SNAPSHOT — copy order data out by id */
bool ob_get_order(const ob_book_t *book, ob_order_id_t id, ob_order_t *out)
{
    uint16_t oi = lookup_find(id);
    if (oi == OB_NULL_IDX) return false;
    *out = book->order_pool[oi];
    return true;
}

/* ─── DISPLAY — pretty-print book state to stdout ───────────────────────── */

typedef struct { uint16_t *arr; uint32_t n; } level_list_t;

static void collect_asc(ob_book_t *b, uint16_t root, level_list_t *lst)
{
    if (root == OB_NULL_IDX) return;
    collect_asc(b, LV(b, root)->left_idx, lst);
    lst->arr[lst->n++] = root;
    collect_asc(b, LV(b, root)->right_idx, lst);
}

static void collect_desc(ob_book_t *b, uint16_t root, level_list_t *lst)
{
    if (root == OB_NULL_IDX) return;
    collect_desc(b, LV(b, root)->right_idx, lst);
    lst->arr[lst->n++] = root;
    collect_desc(b, LV(b, root)->left_idx, lst);
}

/* TOP LEVELS — best `depth` price levels for one side, best-first. */
uint32_t ob_top_levels(const ob_book_t *book, ob_side_t side, uint32_t depth,
                       ob_price_t *prices_out, ob_qty_t *qtys_out)
{
    static uint16_t buf[OB_MAX_LEVELS];
    ob_book_t *b = (ob_book_t *)book;  /* cast away const for LV macro */
    level_list_t lst = { buf, 0 };

    /* collect_asc gives ascending price order (index 0 = lowest = best ask).
     * collect_desc gives descending price order (index 0 = highest = best
     * bid). Both already land index 0 on the correct "best" side. */
    if (side == OB_SIDE_ASK)
        collect_asc(b, book->sides[OB_SIDE_ASK].root_idx, &lst);
    else
        collect_desc(b, book->sides[OB_SIDE_BID].root_idx, &lst);

    uint32_t n = (depth < lst.n) ? depth : lst.n;
    for (uint32_t i = 0; i < n; i++) {
        ob_level_t *lv = LV(b, lst.arr[i]);
        prices_out[i] = lv->price;
        qtys_out[i]   = lv->total_qty;
    }
    return n;
}

void ob_print(const ob_book_t *book, uint32_t max_levels)
{
    static uint16_t buf[OB_MAX_LEVELS];
    ob_book_t *b = (ob_book_t *)book;  /* cast away const for LV macro */

    printf("\n+==========================================+\n");
    printf("|  Order Book: %-28s|\n", book->symbol);
    printf("+==========================================+\n");

    level_list_t asks = { buf, 0 };
    collect_asc(b, book->sides[OB_SIDE_ASK].root_idx, &asks);

    printf("|  %-8s  %-10s  %-10s       |\n", "PRICE", "QTY", "ORDERS");
    printf("|  ASK side                                |\n");

    uint32_t ask_start = (max_levels && asks.n > max_levels) ? asks.n - max_levels : 0;
    for (uint32_t i = asks.n; i-- > ask_start; ) {
        ob_level_t *lv = LV(b, asks.arr[i]);
        printf("|  %-8.4f  %-10llu  %-10u       |\n",
               OB_PRICE_TO_DOUBLE(lv->price),
               (unsigned long long)lv->total_qty,
               lv->order_count);
    }

    ob_price_t spread = ob_spread(book);
    ob_price_t mid    = ob_mid_price(book);
    if (spread >= 0)
        printf("|  --- spread: %-6.4f  mid: %-8.4f ---  |\n",
               OB_PRICE_TO_DOUBLE(spread), OB_PRICE_TO_DOUBLE(mid));
    else
        printf("|  --- (book empty on one or both sides) ---|\n");

    level_list_t bids = { buf, 0 };
    collect_desc(b, book->sides[OB_SIDE_BID].root_idx, &bids);

    uint32_t bid_end = (max_levels && bids.n > max_levels) ? max_levels : bids.n;
    for (uint32_t i = 0; i < bid_end; ++i) {
        ob_level_t *lv = LV(b, bids.arr[i]);
        printf("|  %-8.4f  %-10llu  %-10u       |\n",
               OB_PRICE_TO_DOUBLE(lv->price),
               (unsigned long long)lv->total_qty,
               lv->order_count);
    }
    printf("|  BID side                                |\n");
    printf("+==========================================+\n\n");
}

void ob_print_stats(const ob_book_t *book)
{
    printf("-- Stats [%s] ---------------------\n", book->symbol);
    printf("  Orders added:     %llu\n", (unsigned long long)book->total_orders_added);
    printf("  Orders cancelled: %llu\n", (unsigned long long)book->total_orders_cancelled);
    printf("  Bid levels:       %u\n",   book->sides[OB_SIDE_BID].level_count);
    printf("  Ask levels:       %u\n",   book->sides[OB_SIDE_ASK].level_count);
    printf("  Resting bid qty:  %llu\n", (unsigned long long)book->sides[OB_SIDE_BID].total_qty);
    printf("  Resting ask qty:  %llu\n", (unsigned long long)book->sides[OB_SIDE_ASK].total_qty);
    printf("  Pool orders free: %u / %u\n", book->order_free_top, OB_MAX_ORDERS);
    printf("  Pool levels free: %u / %u\n", book->level_free_top, OB_MAX_LEVELS * OB_SIDE_COUNT);
    printf("----------------------------------------\n\n");
}

