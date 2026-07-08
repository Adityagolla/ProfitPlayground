#ifndef GATEWAY_H
#define GATEWAY_H

/*
 * gateway.h — Order Intake Gateway (the central entry point)
 *
 * The Gateway is NOT a single function. It is a 3-stage component:
 *
 *     ┌──────────────────────────────────────────┐
 *     │  Stage A — Ingress Adapter               │  parse / decode / auth
 *     │  Stage B — Normalizer                    │  canonical message form
 *     │  Stage C — Admission Control             │  rate-limit, dedup, IDs, ts
 *     └──────────────────────────────────────────┘
 *                       │
 *                       ▼  (push into a stage ring — non-blocking)
 *                  Validation → Risk → Sequencer/WAL → Router → ME …
 *
 * Contract of the central entry function `gateway_submit`:
 *   - Synchronous ack, asynchronous execution
 *   - Idempotent on client_order_id (duplicates return original ack)
 *   - Monotonic timestamp assigned on entry
 *   - Bounded latency: returns THROTTLED instead of blocking
 *   - No matching, no portfolio mutation here
 */

#include "matching_engine.h"   /* me_order_type_t, ob_side_t, ob_price_t, ob_qty_t */
#include <stdint.h>
#include <stdbool.h>

/* ─── Source channel (where the order came from) ────────────────────────── */
typedef enum {
    GW_SRC_API   = 1,
    GW_SRC_WS    = 2,
    GW_SRC_FIX   = 3,
    GW_SRC_BOT   = 4,
    GW_SRC_ADMIN = 5,
} gw_source_t;

/* ─── Action (what the client wants) ────────────────────────────────────── */
typedef enum {
    GW_ACT_NEW    = 1,    /* new order */
    GW_ACT_CANCEL = 2,    /* cancel by id */
    GW_ACT_MODIFY = 3,    /* amend price/qty */
} gw_action_t;

/* ─── Raw inbound message (whatever the wire decoder produces) ──────────── */
/*
 * The shape callers MUST use. Adapters (HTTP/WS/FIX/Bot) are responsible
 * for translating their wire format into this struct before calling
 * gateway_submit().
 */
typedef struct {
    gw_source_t      source;
    gw_action_t      action;

    /* Idempotency / correlation */
    uint64_t         client_order_id;   /* unique per account; dedup key   */
    uint32_t         account_id;

    /* For NEW orders */
    me_order_type_t  type;              /* LIMIT / MARKET / IOC / FOK / STOP / STOP_LIMIT */
    ob_side_t        side;
    ob_price_t       price;             /* limit price (0 for market)      */
    ob_price_t       trigger_price;     /* stops only                      */
    ob_qty_t         qty;
    uint64_t         ttl_ns;            /* GTC if 0                        */

    /* For CANCEL / MODIFY */
    ob_order_id_t    target_order_id;   /* the server-assigned id          */
    ob_price_t       new_price;         /* MODIFY only                     */
    ob_qty_t         new_qty;           /* MODIFY only                     */
} gw_raw_msg_t;

/* ─── Acknowledgement returned synchronously to the caller ─────────────── */
typedef enum {
    GW_ACK_ACCEPTED   = 0,    /* admitted into the pipeline                 */
    GW_ACK_REJECTED   = 1,    /* schema/admission rejected                  */
    GW_ACK_DUPLICATE  = 2,    /* client_order_id seen before (idempotent)   */
    GW_ACK_THROTTLED  = 3,    /* rate-limit / queue full                    */
    GW_ACK_KILLED     = 4,    /* kill-switch engaged                        */
} gw_ack_status_t;

typedef struct {
    gw_ack_status_t status;
    uint64_t        server_order_id;    /* assigned by gateway, never reused */
    uint64_t        ingress_ts_ns;      /* monotonic clock at entry         */
    uint64_t        ack_ts_ns;          /* monotonic clock at ack            */
    const char     *reject_reason;      /* points to a string literal       */
} gw_ack_t;

/* ─── Normalized order (what the gateway pushes downstream) ─────────────── */
/*
 * After Stage A/B/C run, the message becomes a normalized order_event_t.
 * Everything downstream consumes this — never the raw_msg.
 */
typedef struct {
    /* Identity */
    uint64_t         server_order_id;
    uint64_t         client_order_id;
    uint32_t         account_id;
    gw_source_t      source;
    gw_action_t      action;

    /* Order body */
    me_order_type_t  type;
    ob_side_t        side;
    ob_price_t       price;
    ob_price_t       trigger_price;
    ob_qty_t         qty;
    uint64_t         ttl_ns;

    /* Modify/Cancel target */
    ob_order_id_t    target_order_id;
    ob_price_t       new_price;
    ob_qty_t         new_qty;

    /* Latency tracking — every stage timestamps itself */
    uint64_t         t_ingress_ns;
    uint64_t         t_normalized_ns;
    uint64_t         t_admitted_ns;
    uint64_t         t_validated_ns;
    uint64_t         t_risk_ns;
    uint64_t         t_sequenced_ns;
    uint64_t         t_matched_ns;

    /* Sequencer assigns this just before the matching engine */
    uint64_t         seq_no;
} order_event_t;

/* ─── Stage ring — SPSC-shaped fixed-capacity queue ─────────────────────── */
/*
 * Single-producer single-consumer ring. Power-of-two capacity. The fields
 * are laid out so head/tail can be made _Atomic later for true lock-free
 * threading without changing call sites.
 */
#define GW_RING_CAP  4096

typedef struct {
    order_event_t  buf[GW_RING_CAP];
    uint32_t       head;     /* producer */
    uint32_t       tail;     /* consumer */
    uint64_t       dropped;
} gw_ring_t;

/* Push an event. Returns false if the ring is full (caller should THROTTLE). */
bool gw_ring_push(gw_ring_t *r, const order_event_t *ev);

/* Pop the oldest event. Returns false if empty. */
bool gw_ring_pop(gw_ring_t *r, order_event_t *out);

/* ─── Idempotency cache ─────────────────────────────────────────────────── */
/*
 * Open-addressed hash map keyed by (account_id, client_order_id). Used to
 * detect duplicate submissions and replay the original ack. Fixed capacity
 * so we never allocate at runtime.
 */
#define GW_IDEM_CAP   8192   /* must be power of two */

typedef struct {
    uint32_t  account_id;
    uint64_t  client_order_id;     /* 0 = empty slot */
    uint64_t  server_order_id;     /* original ack id to replay */
} gw_idem_slot_t;

typedef struct {
    gw_idem_slot_t slots[GW_IDEM_CAP];
    uint32_t       count;
} gw_idem_cache_t;

/* ─── Gateway state ─────────────────────────────────────────────────────── */
typedef struct {
    /* ID generation — strictly monotonic */
    uint64_t         next_server_order_id;

    /* Outbound stage ring (consumed by validation stage) */
    gw_ring_t        out_ring;

    /* Idempotency cache */
    gw_idem_cache_t  idem;

    /* Operational toggles */
    bool             kill_switch;     /* if true, every NEW is rejected     */

    /* Per-account simple token-bucket rate limit (1 bucket for demo) */
    uint64_t         last_refill_ns;
    uint32_t         tokens;
    uint32_t         max_tokens;
    uint32_t         refill_per_sec;

    /* Telemetry */
    uint64_t         accepted;
    uint64_t         rejected;
    uint64_t         duplicates;
    uint64_t         throttled;
} gateway_t;

/* ─── Public API ────────────────────────────────────────────────────────── */

/* Initialise the gateway. Sets all counters to 0 and arms the rate limiter. */
void gateway_init(gateway_t *g);

/* Toggle the global kill switch (e.g. exchange halt, panic stop). */
void gateway_set_kill_switch(gateway_t *g, bool on);

/*
 * gateway_submit — THE central entry point.
 *
 * Runs Stage A → Stage B → Stage C synchronously, then enqueues the
 * normalized event into out_ring for the next pipeline stage. Returns a
 * gw_ack_t to the caller without waiting for matching.
 *
 * This function NEVER:
 *   - mutates the order book
 *   - mutates the portfolio
 *   - blocks on I/O
 *   - allocates heap memory
 */
gw_ack_t gateway_submit(gateway_t *g, const gw_raw_msg_t *msg);

/* Monotonic clock used by the gateway. Exposed so other stages can stamp. */
uint64_t gw_now_ns(void);

#endif /* GATEWAY_H */
