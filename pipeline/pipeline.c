/* Enable C99 printf format specifiers on MinGW (matches matching_engine.c) */
#if defined(__MINGW32__) || defined(__MINGW64__)
#define __USE_MINGW_ANSI_STDIO 1
#endif

/*
 * pipeline.c — End-to-end orchestrator
 *
 * Each stage is a small static function; pipeline_step() is the single
 * place that calls them in order. Keeping the wiring explicit (rather than
 * generic stage callbacks) makes the data flow trivial to read and debug.
 */

#include "pipeline.h"
#include "orders.h"
#include <string.h>
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* id_map_remember — track who owns a server_order_id for fill attribution. */
static void id_map_remember(pipeline_t *pl,
                            uint64_t    server_order_id,
                            uint32_t    account_id,
                            ob_side_t   side,
                            ob_price_t  price,
                            ob_qty_t    qty)
{
    if (pl->id_map_count >= PIPELINE_MAX_ID_MAP) return;     /* capacity cap */
    uint32_t i = pl->id_map_count++;
    pl->id_map[i].server_order_id = server_order_id;
    pl->id_map[i].account_id      = account_id;
    pl->id_map[i].side             = side;
    pl->id_map[i].price            = price;
    pl->id_map[i].qty              = qty;
}

/* id_map_lookup_account — find which account owns a given order id. */
static uint32_t id_map_lookup_account(const pipeline_t *pl, uint64_t id)
{
    for (uint32_t i = 0; i < pl->id_map_count; i++)
        if (pl->id_map[i].server_order_id == id) return pl->id_map[i].account_id;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Stage: Validation (deep, type-specific)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Reuses orders.c's validators implicitly by re-running the same rules. */
static const char *stage_validate(const order_event_t *ev)
{
    if (ev->action != GW_ACT_NEW) return NULL;     /* cancels ok */

    switch (ev->type) {
    case ME_ORDER_LIMIT:
    case ME_ORDER_IOC:
    case ME_ORDER_FOK:
        if (ev->price == 0) return "limit-style order requires price > 0";
        break;
    case ME_ORDER_MARKET:
        if (ev->price != 0) return "market order must have price = 0";
        break;
    case ME_ORDER_STOP:
        if (ev->trigger_price == 0) return "stop requires trigger_price > 0";
        if (ev->price != 0)         return "stop-market must have price = 0";
        break;
    case ME_ORDER_STOP_LIMIT:
        if (ev->trigger_price == 0) return "stop-limit requires trigger_price > 0";
        if (ev->price == 0)         return "stop-limit requires limit price > 0";
        break;
    default:
        return "unknown order type";
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Stage: Sequencer + WAL
 *
 * Assigns a strict monotonic seq_no and appends to the journal. With this
 * in place the system is fully deterministic: re-feed the WAL into a fresh
 * engine and you get the same book state.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void stage_sequence(pipeline_t *pl, order_event_t *ev)
{
    ev->seq_no         = ++pl->next_seq_no;
    ev->t_sequenced_ns = gw_now_ns();
    if (pl->wal) {
        /* Compact line format: easy to grep, easy to replay. */
        fprintf(pl->wal,
                "%llu,%llu,%u,%d,%d,%lld,%lld,%llu,%llu\n",
                (unsigned long long)ev->seq_no,
                (unsigned long long)ev->server_order_id,
                ev->account_id,
                (int)ev->action,
                (int)ev->type,
                (long long)ev->price,
                (long long)ev->trigger_price,
                (unsigned long long)ev->qty,
                (unsigned long long)ev->t_ingress_ns);
        fflush(pl->wal);
    }
    pl->sequenced++;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Stage: Router + Matching
 *
 * Single matching engine for the demo, but the dispatch is centralised here
 * so adding per-symbol shards later is a one-line change.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* match_new — dispatch a NEW order to the right ME entry point. */
static void match_new(pipeline_t *pl, order_event_t *ev, me_result_t *out)
{
    memset(out, 0, sizeof(*out));
    if (ev->type == ME_ORDER_STOP || ev->type == ME_ORDER_STOP_LIMIT) {
        me_submit_stop(&pl->engine, ev->type, ev->side,
                       ev->trigger_price, ev->price, ev->qty,
                       ev->ttl_ns, out);
    } else {
        me_submit_order(&pl->engine, ev->type, ev->side,
                        ev->price, ev->qty, out);
    }
}

/* match_cancel — route a cancel to the appropriate ME path (book or stop). */
static bool match_cancel(pipeline_t *pl, ob_order_id_t target_id)
{
    if (me_cancel_order(&pl->engine, target_id)) return true;
    return me_cancel_stop(&pl->engine, target_id);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Stage: Trade Execution + Portfolio + Bus
 *
 * Drains the ME's outbound event queue. For every TRADE event we update
 * both sides' portfolios. For every order event we publish to the bus.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* publish_me_event — fan one matching-engine event out on the bus. */
static void publish_me_event(pipeline_t *pl, const me_event_t *ev)
{
    bus_topic_t topic;
    switch (ev->type) {
    case ME_EVENT_TRADE:        topic = BUS_TOPIC_TRADE; break;
    case ME_EVENT_BOOK_UPDATED: topic = BUS_TOPIC_BOOK;  break;
    default:                    topic = BUS_TOPIC_ORDER; break;
    }
    bus_msg_t msg = {
        .topic      = topic,
        .ts_ns      = ev->ts,
        .account_id = id_map_lookup_account(pl, ev->order_id),
        .payload    = ev,
    };
    bus_publish(&pl->bus, &msg);
}

/*
 * apply_trade — update both legs' portfolios on a TRADE event.
 *
 * The aggressor is the order that just hit the book. The resting side is
 * looked up via id_map. If either side is "unknown" (e.g. external taker
 * that isn't tracked) the update is silently skipped for that leg.
 */
static void apply_trade(pipeline_t *pl, const me_event_t *ev)
{
    const me_trade_t *t = &ev->trade;

    /* Aggressor leg */
    uint32_t agg_acct = id_map_lookup_account(pl, t->aggressor_id);
    if (agg_acct) {
        portfolio_t *p = pipeline_account(pl, agg_acct);
        if (p) portfolio_on_fill(p, t->aggressor_side, t->price, t->qty);
    }

    /* Resting leg fills on the opposite side */
    uint32_t rest_acct = id_map_lookup_account(pl, t->resting_id);
    if (rest_acct) {
        portfolio_t *p = pipeline_account(pl, rest_acct);
        ob_side_t opp  = (t->aggressor_side == OB_SIDE_BID) ? OB_SIDE_ASK
                                                            : OB_SIDE_BID;
        if (p) portfolio_on_fill(p, opp, t->price, t->qty);
    }

    /* Update risk's reference price. */
    risk_on_trade(&pl->risk, t->price);
}

/*
 * drain_engine_events — pull every queued ME event, route to portfolio + bus.
 *
 * The ME's eq is a public struct; we pop directly so we get full control
 * over routing instead of going through me_run_events()'s handler hook.
 */
static void drain_engine_events(pipeline_t *pl)
{
    me_event_queue_t *q = &pl->engine.eq;
    while (q->tail != q->head) {
        me_event_t ev = q->events[q->tail];
        q->tail = (q->tail + 1) & (ME_EVENT_QUEUE_CAP - 1);

        if (ev.type == ME_EVENT_TRADE) apply_trade(pl, &ev);
        publish_me_event(pl, &ev);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

void pipeline_init(pipeline_t *pl, const char *symbol)
{
    memset(pl, 0, sizeof(*pl));
    gateway_init(&pl->gateway);
    risk_init(&pl->risk);
    bus_init(&pl->bus);
    me_init(&pl->engine, symbol, NULL, NULL);
}

void pipeline_set_wal(pipeline_t *pl, const char *path)
{
    if (pl->wal) { fclose(pl->wal); pl->wal = NULL; }
    if (path)    pl->wal = fopen(path, "w");
}

void pipeline_add_account(pipeline_t *pl,
                          uint32_t    account_id,
                          int64_t     starting_cash,
                          int64_t     max_position,
                          int64_t     max_order_notional)
{
    if (account_id == 0 || account_id > PIPELINE_MAX_ACCOUNTS) return;
    portfolio_init(&pl->accounts[account_id - 1],
                   account_id, starting_cash,
                   max_position, max_order_notional);
}

portfolio_t *pipeline_account(pipeline_t *pl, uint32_t account_id)
{
    if (account_id == 0 || account_id > PIPELINE_MAX_ACCOUNTS) return NULL;
    portfolio_t *p = &pl->accounts[account_id - 1];
    if (p->account_id == 0) return NULL;
    return p;
}

int pipeline_subscribe(pipeline_t   *pl,
                       uint32_t      topic_mask,
                       bus_handler_fn handler,
                       void         *ctx)
{
    return bus_subscribe(&pl->bus, topic_mask, handler, ctx);
}

gw_ack_t pipeline_submit(pipeline_t *pl, const gw_raw_msg_t *msg)
{
    gw_ack_t ack = gateway_submit(&pl->gateway, msg);

    /* Mirror gateway-level events to the bus so subscribers can see
     * rejections / throttles too (UI badges, alerts, etc.). */
    bus_msg_t bm = {
        .topic      = BUS_TOPIC_GATEWAY,
        .ts_ns      = ack.ack_ts_ns,
        .account_id = msg ? msg->account_id : 0,
        .payload    = &ack,
    };
    bus_publish(&pl->bus, &bm);
    return ack;
}

/*
 * pipeline_step — the consumer side. Pops events the gateway parked in
 * out_ring and walks them through every remaining stage.
 */
uint32_t pipeline_step(pipeline_t *pl)
{
    uint32_t processed = 0;
    order_event_t ev;
    while (gw_ring_pop(&pl->gateway.out_ring, &ev)) {
        processed++;

        /* ── Validation ─────────────────────────────────────────────── */
        const char *vmsg = stage_validate(&ev);
        if (vmsg) {
            /* Reject downstream — surface via bus so subscribers learn. */
            bus_msg_t m = { .topic = BUS_TOPIC_GATEWAY,
                            .ts_ns = gw_now_ns(),
                            .account_id = ev.account_id,
                            .payload = vmsg };
            bus_publish(&pl->bus, &m);
            continue;
        }
        ev.t_validated_ns = gw_now_ns();
        pl->validated++;

        /* ── Risk ───────────────────────────────────────────────────── */
        portfolio_t *p = pipeline_account(pl, ev.account_id);
        if (p) {
            const char *rmsg = NULL;
            if (!risk_check(&pl->risk, p, &ev, &rmsg)) {
                bus_msg_t m = { .topic = BUS_TOPIC_GATEWAY,
                                .ts_ns = gw_now_ns(),
                                .account_id = ev.account_id,
                                .payload = rmsg };
                bus_publish(&pl->bus, &m);
                pl->risk_rejected++;
                continue;
            }
        }
        ev.t_risk_ns = gw_now_ns();

        /* ── Sequencer + WAL ─────────────────────────────────────────── */
        stage_sequence(pl, &ev);

        /* ── Router + Matching ──────────────────────────────────────── */
        if (ev.action == GW_ACT_NEW) {
            me_result_t r;
            match_new(pl, &ev, &r);
            ev.t_matched_ns = gw_now_ns();
            pl->matched++;

            /* Remember the assigned ME order id for fill attribution. */
            if (r.id != 0)
                id_map_remember(pl, r.id, ev.account_id,
                                ev.side, ev.price, ev.qty);
        } else if (ev.action == GW_ACT_CANCEL) {
            match_cancel(pl, ev.target_order_id);
        } else if (ev.action == GW_ACT_MODIFY) {
            /* Book-level amend: emits ME_EVENT_ORDER_MODIFIED on success.
             * Only resting book orders are modifiable (not dormant stops). */
            me_modify_order(&pl->engine, ev.target_order_id,
                            ev.new_price, ev.new_qty);
        }

        /* ── Trade execution + portfolio + bus fan-out ──────────────── */
        drain_engine_events(pl);
    }
    return processed;
}

void pipeline_print_stats(const pipeline_t *pl)
{
    printf("\n──────── Pipeline Stats ────────\n");
    printf(" Gateway accepted : %llu\n", (unsigned long long)pl->gateway.accepted);
    printf(" Gateway rejected : %llu\n", (unsigned long long)pl->gateway.rejected);
    printf(" Gateway dup      : %llu\n", (unsigned long long)pl->gateway.duplicates);
    printf(" Gateway throttled: %llu\n", (unsigned long long)pl->gateway.throttled);
    printf(" Validated        : %llu\n", (unsigned long long)pl->validated);
    printf(" Risk rejected    : %llu\n", (unsigned long long)pl->risk.rejected);
    printf(" Sequenced        : %llu\n", (unsigned long long)pl->sequenced);
    printf(" Matched          : %llu\n", (unsigned long long)pl->matched);
    printf(" ME total trades  : %llu\n", (unsigned long long)pl->engine.total_trades);
    printf(" Bus published    : %llu\n", (unsigned long long)pl->bus.published);
    printf(" Bus delivered    : %llu\n", (unsigned long long)pl->bus.delivered);
    printf("────────────────────────────────\n");
}
