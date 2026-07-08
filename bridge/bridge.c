/*
 * bridge.c — Implementation of bridge.h.
 *
 * Owns one pipeline_t per symbol (see bridge_pipeline_t below), each with
 * its own order registry (for GET /orders/{id} semantics the underlying
 * engine doesn't retain on its own) and its own event queue (drained by
 * bridge_poll_event).
 *
 * Order-id namespacing: gateway_t assigns ids starting from 1 per pipeline
 * instance, so two symbols would otherwise both hand out order_id=1,2,3...
 * and collide once exposed through one flat GET /orders/{id} endpoint.
 * Each symbol's gateway counter is seeded with a large offset
 * (BRIDGE_ORDER_ID_SPACE * symbol_index) so ids stay globally unique.
 */

#include "bridge.h"
#include "pipeline.h"

#include <stdlib.h>
#include <string.h>

#define BRIDGE_MAX_SYMBOLS       64
#define BRIDGE_ORDER_ID_SPACE    1000000ULL
#define BRIDGE_EVENT_QUEUE_CAP   4096
#define BRIDGE_ORDER_REGISTRY_CAP PIPELINE_MAX_ID_MAP
#define BRIDGE_STAGE_CAP         1024   /* >= ME_EVENT_QUEUE_CAP           */

typedef struct {
    uint64_t order_id;         /* 0 = empty slot (gateway/server id)       */
    uint64_t me_id;            /* matching-engine id (0 until confirmed)   */
    uint64_t client_order_id;
    uint32_t account_id;
    int32_t  type;
    int32_t  side;
    int64_t  price;
    int64_t  trigger_price;
    uint64_t qty_original, qty_remain, qty_filled;
    int32_t  status;
    uint64_t created_ts_ns, updated_ts_ns;
    bool     touched;   /* has an ME event updated this since submission?  */
} bridge_order_slot_t;

typedef struct {
    pipeline_t            pl;
    char                  symbol[OB_MAX_SYMBOL_LEN];
    uint64_t              id_base;   /* symbol_index * BRIDGE_ORDER_ID_SPACE */
    bridge_order_slot_t  *orders;    /* calloc'd, BRIDGE_ORDER_REGISTRY_CAP entries */

    /* ME order-id -> gateway/server order-id. The gateway and the matching
     * engine each run their own id counters (gateway ids carry the symbol's
     * id_base offset, ME ids always start at 1), so every id that crosses
     * the bus has to be translated before it is exposed. Indexed me_id-1. */
    uint64_t             *me_to_gw;  /* calloc'd, BRIDGE_ORDER_REGISTRY_CAP entries */

    /* Raw ME events captured by the bus callback during pipeline_step().
     * They are translated + published only after bridge_submit has recorded
     * the me_id<->gw_id mapping for the order that was just submitted. */
    me_event_t            staged[BRIDGE_STAGE_CAP];
    uint32_t              staged_count;

    bridge_event_t        evq[BRIDGE_EVENT_QUEUE_CAP];
    uint32_t              evq_head, evq_tail;
    uint64_t              evq_dropped;
    uint64_t              next_seq;
} bridge_pipeline_t;

static bridge_pipeline_t *g_symbols[BRIDGE_MAX_SYMBOLS];
static uint32_t           g_symbol_count = 0;

/* ─── Registry + queue helpers ───────────────────────────────────────────── */

static bridge_order_slot_t *slot_for(bridge_pipeline_t *bp, uint64_t order_id)
{
    if (!bp || order_id == 0 || order_id <= bp->id_base) return NULL;
    uint64_t local = order_id - bp->id_base - 1;
    if (local >= BRIDGE_ORDER_REGISTRY_CAP) return NULL;
    bridge_order_slot_t *s = &bp->orders[local];
    if (s->order_id != order_id) return NULL;
    return s;
}

/* Drop-oldest on overflow: never let a slow Python-side poller stall the
 * matching engine, mirroring the drop policy already used by gw_ring_t and
 * the Python mock's own event queue. */
static void push_event(bridge_pipeline_t *bp, bridge_event_t ev)
{
    ev.seq = ++bp->next_seq;
    uint32_t next = (bp->evq_head + 1) % BRIDGE_EVENT_QUEUE_CAP;
    if (next == bp->evq_tail) {
        bp->evq_tail = (bp->evq_tail + 1) % BRIDGE_EVENT_QUEUE_CAP;
        bp->evq_dropped++;
    }
    bp->evq[bp->evq_head] = ev;
    bp->evq_head = next;
}

/* gw id for an ME id, or 0 when unmapped (ME rejections carry id 0). */
static uint64_t gw_from_me(bridge_pipeline_t *bp, uint64_t me_id)
{
    if (me_id == 0 || me_id > BRIDGE_ORDER_REGISTRY_CAP) return 0;
    return bp->me_to_gw[me_id - 1];
}

/* ─── Bus subscriber: fired synchronously from inside pipeline_step() ─────
 *
 * Only stages a copy of the raw ME event. Translation to gateway ids and
 * slot updates happen in process_staged_events(), because the me_id of the
 * order being submitted is only learnable AFTER pipeline_step returns (from
 * the pipeline's id_map) — events for that very order fire during the step. */

static void on_bus_event(const bus_msg_t *msg, void *ctx)
{
    bridge_pipeline_t *bp = (bridge_pipeline_t *)ctx;
    if (msg->topic != BUS_TOPIC_ORDER && msg->topic != BUS_TOPIC_TRADE) return;

    const me_event_t *ev = (const me_event_t *)msg->payload;
    if (ev->type == ME_EVENT_BOOK_UPDATED) return;   /* not order-scoped */

    if (bp->staged_count < BRIDGE_STAGE_CAP)
        bp->staged[bp->staged_count++] = *ev;
    else
        bp->evq_dropped++;
}

/* Update a slot from one fill and return its new status. */
static int32_t slot_apply_fill(bridge_order_slot_t *s, uint64_t qty,
                               uint64_t ts)
{
    if (!s) return BRIDGE_STATUS_OPEN;
    s->qty_filled += qty;
    s->qty_remain  = (s->qty_remain >= qty) ? s->qty_remain - qty : 0;
    s->status      = (s->qty_remain == 0) ? BRIDGE_STATUS_FILLED
                                          : BRIDGE_STATUS_PARTIAL;
    s->updated_ts_ns = ts;
    s->touched = true;
    return s->status;
}

static void emit_order_snapshot(bridge_pipeline_t *bp,
                                const bridge_order_slot_t *s, uint64_t ts)
{
    bridge_event_t out;
    memset(&out, 0, sizeof(out));
    out.kind         = BRIDGE_EVENT_ORDER;
    out.ts_ns        = ts;
    out.order_id     = s->order_id;
    out.account_id   = s->account_id;
    out.side         = s->side;
    out.type         = s->type;
    out.price        = s->price;
    out.qty_original = s->qty_original;
    out.qty_remain   = s->qty_remain;
    out.qty_filled   = s->qty_filled;
    out.status       = s->status;
    push_event(bp, out);
}

/* Translate + publish everything staged during the last pipeline_step(). */
static void process_staged_events(bridge_pipeline_t *bp)
{
    for (uint32_t i = 0; i < bp->staged_count; i++) {
        const me_event_t *ev = &bp->staged[i];

        if (ev->type == ME_EVENT_TRADE) {
            const me_trade_t *t = &ev->trade;
            uint64_t agg_gw  = gw_from_me(bp, t->aggressor_id);
            uint64_t rest_gw = gw_from_me(bp, t->resting_id);
            bridge_order_slot_t *agg  = slot_for(bp, agg_gw);
            bridge_order_slot_t *rest = slot_for(bp, rest_gw);

            /* The ME emits a status event for the aggressor but never for
             * the resting order, so both slots are advanced from the trade
             * itself (the aggressor's own event lands later and agrees). */
            slot_apply_fill(agg,  t->qty, t->ts);
            slot_apply_fill(rest, t->qty, t->ts);

            bridge_event_t out;
            memset(&out, 0, sizeof(out));
            out.kind                 = BRIDGE_EVENT_TRADE;
            out.ts_ns                = t->ts;
            out.trade_id             = t->trade_id;
            out.aggressor_id         = agg_gw;
            out.resting_id           = rest_gw;
            out.aggressor_side       = t->aggressor_side;
            out.trade_price          = t->price;
            out.trade_qty            = t->qty;
            out.aggressor_account_id = agg  ? agg->account_id  : 0;
            out.resting_account_id   = rest ? rest->account_id : 0;
            push_event(bp, out);

            /* Surface the resting side's new state as an order event too,
             * so downstream order views / DB snapshots stay accurate. */
            if (rest) emit_order_snapshot(bp, rest, t->ts);
            continue;
        }

        /* Order-scoped event. */
        uint64_t gw_id = gw_from_me(bp, ev->order_id);
        if (gw_id == 0) continue;   /* ME rejection (id 0) or unmapped —
                                     * the synthetic-reject path in
                                     * bridge_submit covers the slot. */
        bridge_order_slot_t *slot = slot_for(bp, gw_id);

        /* CANCELLED/REJECTED are terminal. The ME emits a follow-up FILLED
         * event after cancelling an IOC remainder (its qty_remain hit 0 in
         * the same submit) — don't let that resurrect a dead order. */
        if (slot && slot->touched &&
            (slot->status == BRIDGE_STATUS_CANCELLED ||
             slot->status == BRIDGE_STATUS_REJECTED))
            continue;

        int32_t status;
        switch (ev->type) {
        case ME_EVENT_ORDER_CANCELLED: status = BRIDGE_STATUS_CANCELLED; break;
        case ME_EVENT_ORDER_REJECTED:  status = BRIDGE_STATUS_REJECTED;  break;
        case ME_EVENT_ORDER_FILLED:    status = BRIDGE_STATUS_FILLED;    break;
        case ME_EVENT_ORDER_PARTIAL:   status = BRIDGE_STATUS_PARTIAL;   break;
        default:
            status = (ev->qty_remain == 0) ? BRIDGE_STATUS_FILLED
                                           : BRIDGE_STATUS_OPEN;
            break;
        }

        if (slot) {
            slot->touched = true;
            slot->updated_ts_ns = ev->ts;
            if (ev->type == ME_EVENT_ORDER_CANCELLED) {
                /* me_cancel_order passes the old remaining qty in the
                 * qty_filled arg slot — don't let it clobber real fills. */
                slot->qty_remain = 0;
            } else if (ev->type == ME_EVENT_ORDER_MODIFIED) {
                /* MODIFIED carries: price = new price, qty_remain = new qty. */
                slot->price      = ev->price;
                slot->qty_remain = ev->qty_remain;
            } else {
                slot->qty_remain = ev->qty_remain;
                if (ev->qty_filled > slot->qty_filled)
                    slot->qty_filled = ev->qty_filled;
            }
            if (status == BRIDGE_STATUS_OPEN && slot->qty_filled > 0)
                status = BRIDGE_STATUS_PARTIAL;
            slot->status = status;
            emit_order_snapshot(bp, slot, ev->ts);
        } else {
            bridge_event_t out;
            memset(&out, 0, sizeof(out));
            out.kind         = BRIDGE_EVENT_ORDER;
            out.ts_ns        = ev->ts;
            out.order_id     = gw_id;
            out.side         = ev->side;
            out.type         = ev->order_type;
            out.price        = ev->price;
            out.qty_original = ev->qty_original;
            out.qty_remain   = ev->qty_remain;
            out.qty_filled   = ev->qty_filled;
            out.status       = status;
            push_event(bp, out);
        }
    }
    bp->staged_count = 0;
}

/* ─── Lifecycle ──────────────────────────────────────────────────────────── */

BRIDGE_API bridge_symbol_t bridge_add_symbol(const char *symbol)
{
    if (!symbol || !symbol[0] || strlen(symbol) >= OB_MAX_SYMBOL_LEN) return NULL;

    for (uint32_t i = 0; i < g_symbol_count; i++) {
        if (strcmp(g_symbols[i]->symbol, symbol) == 0) return g_symbols[i];
    }
    if (g_symbol_count >= BRIDGE_MAX_SYMBOLS) return NULL;

    bridge_pipeline_t *bp = (bridge_pipeline_t *)calloc(1, sizeof(bridge_pipeline_t));
    if (!bp) return NULL;
    bp->orders = (bridge_order_slot_t *)calloc(BRIDGE_ORDER_REGISTRY_CAP,
                                               sizeof(bridge_order_slot_t));
    if (!bp->orders) { free(bp); return NULL; }
    bp->me_to_gw = (uint64_t *)calloc(BRIDGE_ORDER_REGISTRY_CAP,
                                      sizeof(uint64_t));
    if (!bp->me_to_gw) { free(bp->orders); free(bp); return NULL; }

    strncpy(bp->symbol, symbol, OB_MAX_SYMBOL_LEN - 1);

    uint32_t idx = g_symbol_count;
    bp->id_base = (uint64_t)idx * BRIDGE_ORDER_ID_SPACE;

    pipeline_init(&bp->pl, symbol);
    bp->pl.gateway.next_server_order_id = bp->id_base;  /* first id = base+1 */
    pipeline_subscribe(&bp->pl, BUS_TOPIC_ORDER | BUS_TOPIC_TRADE, on_bus_event, bp);

    g_symbols[idx] = bp;
    g_symbol_count++;
    return bp;
}

BRIDGE_API bool bridge_add_account(bridge_symbol_t h, uint32_t account_id,
                                   int64_t cash, int64_t max_position,
                                   int64_t max_order_notional)
{
    bridge_pipeline_t *bp = (bridge_pipeline_t *)h;
    if (!bp) return false;
    pipeline_add_account(&bp->pl, account_id, cash, max_position, max_order_notional);
    return true;
}

/* ─── Order entry ─────────────────────────────────────────────────────────── */

BRIDGE_API bridge_ack_t bridge_submit(bridge_symbol_t h, const bridge_raw_msg_t *msg)
{
    bridge_pipeline_t *bp = (bridge_pipeline_t *)h;
    bridge_ack_t out;
    memset(&out, 0, sizeof(out));

    if (!bp || !msg) {
        out.status = GW_ACK_REJECTED;
        out.reject_reason = "null handle or message";
        return out;
    }

    gw_raw_msg_t gm;
    memset(&gm, 0, sizeof(gm));
    gm.source          = GW_SRC_API;
    gm.action          = (gw_action_t)msg->action;
    gm.client_order_id = msg->client_order_id;
    gm.account_id      = msg->account_id;
    gm.type            = (me_order_type_t)msg->type;
    gm.side            = (ob_side_t)msg->side;
    gm.price           = (ob_price_t)msg->price;
    gm.trigger_price   = (ob_price_t)msg->trigger_price;
    gm.qty             = (ob_qty_t)msg->qty;
    gm.ttl_ns          = msg->ttl_ns;
    gm.new_price       = (ob_price_t)msg->new_price;
    gm.new_qty         = (ob_qty_t)msg->new_qty;

    /* CANCEL/MODIFY targets arrive in the exposed (gateway) id namespace;
     * the matching engine speaks its own ids — translate via the slot. */
    if (gm.action == GW_ACT_CANCEL || gm.action == GW_ACT_MODIFY) {
        bridge_order_slot_t *ts = slot_for(bp, msg->target_order_id);
        if (!ts || ts->me_id == 0) {
            out.status = GW_ACK_REJECTED;
            out.reject_reason = "unknown target order";
            return out;
        }
        gm.target_order_id = (ob_order_id_t)ts->me_id;
    }

    gw_ack_t ack = pipeline_submit(&bp->pl, &gm);

    out.status          = (int32_t)ack.status;
    out.server_order_id = ack.server_order_id;
    out.ingress_ts_ns   = ack.ingress_ts_ns;
    out.ack_ts_ns       = ack.ack_ts_ns;
    out.reject_reason   = ack.reject_reason;

    if (ack.status != GW_ACK_ACCEPTED) return out;  /* nothing pushed downstream */

    /* Pre-register the registry slot for a NEW order BEFORE stepping, so the
     * bus callback (fired synchronously inside pipeline_step) can find it
     * and so we can tell, after stepping, whether it ever got processed. */
    uint64_t local = (gm.action == GW_ACT_NEW && ack.server_order_id > bp->id_base)
                    ? ack.server_order_id - bp->id_base - 1
                    : (uint64_t)BRIDGE_ORDER_REGISTRY_CAP;  /* sentinel: out of range */

    if (gm.action == GW_ACT_NEW && local < BRIDGE_ORDER_REGISTRY_CAP) {
        bridge_order_slot_t *s = &bp->orders[local];
        memset(s, 0, sizeof(*s));
        s->order_id        = ack.server_order_id;
        s->client_order_id = msg->client_order_id;
        s->account_id      = msg->account_id;
        s->type            = msg->type;
        s->side            = msg->side;
        s->price           = msg->price;
        s->trigger_price   = msg->trigger_price;
        s->qty_original    = msg->qty;
        s->qty_remain      = msg->qty;
        s->qty_filled      = 0;
        s->status          = BRIDGE_STATUS_OPEN;
        s->created_ts_ns   = ack.ingress_ts_ns;
        s->updated_ts_ns   = ack.ack_ts_ns;
        s->touched         = false;
    }

    uint32_t prev_idmap = bp->pl.id_map_count;
    pipeline_step(&bp->pl);

    /* A NEW order that reached the matching engine was recorded in the
     * pipeline's id_map with the ME-assigned id. Capture the me<->gw
     * mapping BEFORE processing staged events so they can be translated. */
    if (gm.action == GW_ACT_NEW && local < BRIDGE_ORDER_REGISTRY_CAP &&
        bp->pl.id_map_count > prev_idmap) {
        uint64_t me_id = bp->pl.id_map[bp->pl.id_map_count - 1].server_order_id;
        if (me_id >= 1 && me_id <= BRIDGE_ORDER_REGISTRY_CAP) {
            bp->orders[local].me_id  = me_id;
            bp->me_to_gw[me_id - 1]  = ack.server_order_id;
        }
    }

    process_staged_events(bp);

    /* gw_ack_t only reflects gateway admission (schema/kill-switch/rate
     * limit) -- validation and risk rejections happen later inside
     * pipeline_step and are only visible on the bus as an untargeted
     * BUS_TOPIC_GATEWAY string, with no order_id attached. Detect that
     * case here (the slot was pre-registered but never touched by an
     * ORDER bus event) and surface it as a synthetic REJECTED event so
     * Python's view of the order stays consistent with reality. */
    if (gm.action == GW_ACT_NEW && local < BRIDGE_ORDER_REGISTRY_CAP) {
        bridge_order_slot_t *s = &bp->orders[local];
        if (s->order_id == ack.server_order_id && !s->touched) {
            s->status  = BRIDGE_STATUS_REJECTED;
            s->touched = true;

            bridge_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind         = BRIDGE_EVENT_ORDER;
            ev.ts_ns        = ack.ack_ts_ns;
            ev.order_id     = s->order_id;
            ev.account_id   = s->account_id;
            ev.side         = s->side;
            ev.type         = s->type;
            ev.price        = s->price;
            ev.qty_original = s->qty_original;
            ev.qty_remain   = s->qty_remain;
            ev.qty_filled   = 0;
            ev.status       = BRIDGE_STATUS_REJECTED;
            push_event(bp, ev);
        }
    }

    return out;
}

BRIDGE_API bridge_ack_t bridge_cancel(bridge_symbol_t h, uint64_t order_id,
                                      uint32_t account_id)
{
    bridge_raw_msg_t m;
    memset(&m, 0, sizeof(m));
    m.action          = GW_ACT_CANCEL;
    m.account_id      = account_id;
    m.target_order_id = order_id;
    /* client_order_id left at 0 -> bypasses the idempotency cache entirely,
     * which is exactly what we want for a cancel instruction. */
    return bridge_submit(h, &m);
}

BRIDGE_API bridge_order_view_t bridge_get_order(bridge_symbol_t h, uint64_t order_id)
{
    bridge_pipeline_t *bp = (bridge_pipeline_t *)h;
    bridge_order_view_t out;
    memset(&out, 0, sizeof(out));

    bridge_order_slot_t *s = slot_for(bp, order_id);
    if (!s) { out.found = false; return out; }

    out.found           = true;
    out.server_order_id = s->order_id;
    out.client_order_id = s->client_order_id;
    out.account_id      = s->account_id;
    out.type            = s->type;
    out.side            = s->side;
    out.price           = s->price;
    out.trigger_price   = s->trigger_price;
    out.qty_original    = s->qty_original;
    out.qty_remain      = s->qty_remain;
    out.qty_filled      = s->qty_filled;
    out.status          = s->status;
    out.created_ts_ns   = s->created_ts_ns;
    out.updated_ts_ns   = s->updated_ts_ns;
    return out;
}

/* ─── Market data ─────────────────────────────────────────────────────────── */

BRIDGE_API bridge_tob_t bridge_top_of_book(bridge_symbol_t h)
{
    bridge_pipeline_t *bp = (bridge_pipeline_t *)h;
    bridge_tob_t out;
    memset(&out, 0, sizeof(out));
    if (!bp) return out;

    me_top_of_book_t t = me_top_of_book(&bp->pl.engine);
    out.bid_price = t.bid_price;
    out.bid_qty   = (int64_t)t.bid_qty;
    out.ask_price = t.ask_price;
    out.ask_qty   = (int64_t)t.ask_qty;
    out.spread    = t.spread;
    out.mid       = t.mid;
    return out;
}

BRIDGE_API uint32_t bridge_book_levels(bridge_symbol_t h, int32_t side, uint32_t depth,
                                       int64_t *prices_out, int64_t *qtys_out)
{
    bridge_pipeline_t *bp = (bridge_pipeline_t *)h;
    if (!bp || !prices_out || !qtys_out) return 0;
    if (depth > OB_MAX_LEVELS) depth = OB_MAX_LEVELS;

    ob_price_t prices[OB_MAX_LEVELS];
    ob_qty_t   qtys[OB_MAX_LEVELS];
    uint32_t n = ob_top_levels(&bp->pl.engine.book, (ob_side_t)side, depth, prices, qtys);
    for (uint32_t i = 0; i < n; i++) {
        prices_out[i] = (int64_t)prices[i];
        qtys_out[i]   = (int64_t)qtys[i];
    }
    return n;
}

/* ─── Event stream ─────────────────────────────────────────────────────────── */

BRIDGE_API bool bridge_poll_event(bridge_symbol_t h, bridge_event_t *out)
{
    bridge_pipeline_t *bp = (bridge_pipeline_t *)h;
    if (!bp || !out || bp->evq_tail == bp->evq_head) return false;
    *out = bp->evq[bp->evq_tail];
    bp->evq_tail = (bp->evq_tail + 1) % BRIDGE_EVENT_QUEUE_CAP;
    return true;
}
