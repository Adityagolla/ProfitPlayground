/*
 * gateway.c — Order Intake Gateway implementation
 *
 * Three sub-stages run inside gateway_submit():
 *   Stage A — Ingress Adapter   (basic shape check; the wire decoder ran first)
 *   Stage B — Normalizer        (build the canonical order_event_t)
 *   Stage C — Admission Control (kill-switch, rate-limit, dedup, IDs, ts)
 *
 * On success, the event is pushed into out_ring for the next pipeline stage
 * (validation). On failure, an ack with a reject reason is returned.
 *
 * Everything here is allocation-free and bounded-time.
 */

#include "gateway.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Monotonic clock (Windows + POSIX)
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#include <windows.h>
/* Returns nanoseconds from a monotonic source. Wall-clock is never used here. */
uint64_t gw_now_ns(void)
{
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)(count.QuadPart * 1000000000ULL / (uint64_t)freq.QuadPart);
}
#else
#include <time.h>
uint64_t gw_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Ring buffer (SPSC-shaped — head/tail not yet atomic; ready for upgrade)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define GW_RING_MASK  (GW_RING_CAP - 1)

/* Push one event. Caller must treat false as backpressure (THROTTLE). */
bool gw_ring_push(gw_ring_t *r, const order_event_t *ev)
{
    uint32_t next = (r->head + 1) & GW_RING_MASK;
    if (next == r->tail) { r->dropped++; return false; }
    r->buf[r->head] = *ev;
    r->head = next;
    return true;
}

/* Pop the oldest event. False = empty. */
bool gw_ring_pop(gw_ring_t *r, order_event_t *out)
{
    if (r->tail == r->head) return false;
    *out = r->buf[r->tail];
    r->tail = (r->tail + 1) & GW_RING_MASK;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Idempotency cache — open addressing, linear probing
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 64-bit mix used to spread (account, client_id) across the slot array. */
static uint32_t idem_hash(uint32_t account_id, uint64_t client_order_id)
{
    uint64_t x = ((uint64_t)account_id << 32) ^ client_order_id;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint32_t)(x & (GW_IDEM_CAP - 1));
}

/* Look up an existing entry. Returns the matching slot or NULL. */
static gw_idem_slot_t *idem_find(gw_idem_cache_t *c,
                                 uint32_t account_id,
                                 uint64_t client_order_id)
{
    if (client_order_id == 0) return NULL;   /* 0 = "no idempotency key"   */
    uint32_t i = idem_hash(account_id, client_order_id);
    for (uint32_t probes = 0; probes < GW_IDEM_CAP; probes++) {
        gw_idem_slot_t *s = &c->slots[i];
        if (s->client_order_id == 0) return NULL;     /* hit empty → miss   */
        if (s->client_order_id == client_order_id &&
            s->account_id      == account_id) return s;
        i = (i + 1) & (GW_IDEM_CAP - 1);
    }
    return NULL;
}

/* Insert a new entry. Best-effort: if cache is full, just skip — duplicate
 * detection becomes a no-op rather than blocking trading. */
static void idem_insert(gw_idem_cache_t *c,
                        uint32_t  account_id,
                        uint64_t  client_order_id,
                        uint64_t  server_order_id)
{
    if (client_order_id == 0) return;
    uint32_t i = idem_hash(account_id, client_order_id);
    for (uint32_t probes = 0; probes < GW_IDEM_CAP; probes++) {
        gw_idem_slot_t *s = &c->slots[i];
        if (s->client_order_id == 0) {
            s->account_id       = account_id;
            s->client_order_id  = client_order_id;
            s->server_order_id  = server_order_id;
            c->count++;
            return;
        }
        i = (i + 1) & (GW_IDEM_CAP - 1);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Token-bucket rate limiter (very simple, single bucket)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Refill tokens based on elapsed time. Cheap; called on every submit. */
static void rl_refill(gateway_t *g, uint64_t now_ns)
{
    if (g->last_refill_ns == 0) { g->last_refill_ns = now_ns; return; }
    uint64_t dt   = now_ns - g->last_refill_ns;
    uint64_t add  = dt * g->refill_per_sec / 1000000000ULL;
    if (add == 0) return;
    g->tokens = (uint32_t)((g->tokens + add > g->max_tokens)
                           ? g->max_tokens
                           : g->tokens + add);
    g->last_refill_ns = now_ns;
}

/* Consume one token. Returns false if the bucket is empty (THROTTLED). */
static bool rl_take(gateway_t *g)
{
    if (g->tokens == 0) return false;
    g->tokens--;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Stage A — Ingress Adapter
 *   Cheap structural checks only. Heavier validation lives downstream.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Returns NULL if the raw message is structurally usable, else a reason. */
static const char *stage_a_ingress(const gw_raw_msg_t *m)
{
    if (!m)                           return "null message";
    if (m->action == 0)               return "missing action";
    if (m->source == 0)               return "missing source";
    if (m->account_id == 0)           return "missing account_id";

    if (m->action == GW_ACT_NEW) {
        if (m->qty == 0)              return "qty must be > 0";
        if (m->side >= OB_SIDE_COUNT) return "invalid side";
        /* m->type 0 == ME_ORDER_LIMIT (valid). Only reject unknown types. */
        if (m->type > ME_ORDER_STOP_LIMIT) return "unknown order type";
    } else if (m->action == GW_ACT_CANCEL || m->action == GW_ACT_MODIFY) {
        if (m->target_order_id == 0)  return "missing target_order_id";
    } else {
        return "unknown action";
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Stage B — Normalizer
 *   Convert the raw message into the canonical event used downstream.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Pure transform: copy fields, leave IDs/timestamps for Stage C to fill. */
static void stage_b_normalize(const gw_raw_msg_t *m, order_event_t *ev)
{
    memset(ev, 0, sizeof(*ev));
    ev->client_order_id = m->client_order_id;
    ev->account_id      = m->account_id;
    ev->source          = m->source;
    ev->action          = m->action;
    ev->type            = m->type;
    ev->side            = m->side;
    ev->price           = m->price;
    ev->trigger_price   = m->trigger_price;
    ev->qty             = m->qty;
    ev->ttl_ns          = m->ttl_ns;
    ev->target_order_id = m->target_order_id;
    ev->new_price       = m->new_price;
    ev->new_qty         = m->new_qty;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Stage C — Admission Control
 *   Kill-switch, rate-limit, idempotency, ID assignment, timestamping.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* All the "policy" checks that the gateway owns before pushing downstream. */
static gw_ack_status_t stage_c_admit(gateway_t       *g,
                                     order_event_t   *ev,
                                     uint64_t         t_ingress_ns,
                                     const char     **reason_out,
                                     uint64_t        *server_id_out)
{
    /* 1. Kill-switch: only NEW orders are blocked; cancels still go through. */
    if (g->kill_switch && ev->action == GW_ACT_NEW) {
        *reason_out = "kill-switch engaged";
        return GW_ACK_KILLED;
    }

    /* 2. Rate-limit. */
    rl_refill(g, t_ingress_ns);
    if (!rl_take(g)) {
        *reason_out = "rate limit exceeded";
        return GW_ACK_THROTTLED;
    }

    /* 3. Idempotency: replay original ack on duplicate client_order_id. */
    gw_idem_slot_t *dup = idem_find(&g->idem,
                                    ev->account_id,
                                    ev->client_order_id);
    if (dup) {
        *server_id_out = dup->server_order_id;
        *reason_out    = "duplicate client_order_id";
        return GW_ACK_DUPLICATE;
    }

    /* 4. Assign IDs and timestamps. */
    ev->server_order_id  = ++g->next_server_order_id;
    ev->t_ingress_ns     = t_ingress_ns;
    ev->t_normalized_ns  = gw_now_ns();
    ev->t_admitted_ns    = ev->t_normalized_ns;

    /* 5. Remember the ack for future duplicate detection. */
    idem_insert(&g->idem,
                ev->account_id,
                ev->client_order_id,
                ev->server_order_id);

    *server_id_out = ev->server_order_id;
    return GW_ACK_ACCEPTED;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Zero state and arm a sane default rate limit (10k orders/sec, burst 2k). */
void gateway_init(gateway_t *g)
{
    memset(g, 0, sizeof(*g));
    g->max_tokens     = 2000;
    g->tokens         = 2000;
    g->refill_per_sec = 10000;
}

/* Flip the kill switch. Cancels keep flowing; NEW orders get GW_ACK_KILLED. */
void gateway_set_kill_switch(gateway_t *g, bool on) { g->kill_switch = on; }

/*
 * THE central entry function. Runs A → B → C synchronously, pushes the
 * normalized event into out_ring, returns a gw_ack_t.
 */
gw_ack_t gateway_submit(gateway_t *g, const gw_raw_msg_t *msg)
{
    uint64_t  t_ingress = gw_now_ns();
    gw_ack_t  ack       = (gw_ack_t){0};
    ack.ingress_ts_ns   = t_ingress;

    /* ─── Stage A — Ingress Adapter ─────────────────────────────────── */
    const char *err = stage_a_ingress(msg);
    if (err) {
        g->rejected++;
        ack.status        = GW_ACK_REJECTED;
        ack.reject_reason = err;
        ack.ack_ts_ns     = gw_now_ns();
        return ack;
    }

    /* ─── Stage B — Normalize ───────────────────────────────────────── */
    order_event_t ev;
    stage_b_normalize(msg, &ev);

    /* ─── Stage C — Admission Control ───────────────────────────────── */
    const char *reason   = NULL;
    uint64_t    server_id = 0;
    gw_ack_status_t st = stage_c_admit(g, &ev, t_ingress, &reason, &server_id);

    if (st != GW_ACK_ACCEPTED) {
        if (st == GW_ACK_DUPLICATE) g->duplicates++;
        else if (st == GW_ACK_THROTTLED) g->throttled++;
        else g->rejected++;
        ack.status          = st;
        ack.reject_reason   = reason;
        ack.server_order_id = server_id;
        ack.ack_ts_ns       = gw_now_ns();
        return ack;
    }

    /* ─── Push to out ring (non-blocking; full = THROTTLED) ─────────── */
    if (!gw_ring_push(&g->out_ring, &ev)) {
        g->throttled++;
        ack.status        = GW_ACK_THROTTLED;
        ack.reject_reason = "downstream ring full";
        ack.ack_ts_ns     = gw_now_ns();
        return ack;
    }

    g->accepted++;
    ack.status          = GW_ACK_ACCEPTED;
    ack.server_order_id = server_id;
    ack.ack_ts_ns       = gw_now_ns();
    return ack;
}
