#ifndef PIPELINE_H
#define PIPELINE_H

/*
 * pipeline.h — End-to-end trade lifecycle orchestrator
 *
 *   gateway_submit()                           ← caller's only entry
 *        │
 *        ▼   (out_ring: order_event_t)
 *   pipeline_step()
 *        │
 *        ├─ Validation     (deep schema, type-specific)
 *        ├─ Risk           (margin / fat-finger / tick / portfolio reserve)
 *        ├─ Sequencer+WAL  (assign seq_no, append to journal)
 *        ├─ Router         (pick ME instance — single one in this build)
 *        ├─ Matching       (me_submit_order / me_submit_stop / me_cancel_order)
 *        ├─ Trade Exec     (drain ME events, update portfolio, build bus msgs)
 *        └─ Event Bus      (fan-out to subscribers)
 *
 * Single-threaded, deterministic, replayable. All stage rings are SPSC and
 * laid out so head/tail can be made atomic later for true threading.
 */

#include "gateway.h"
#include "risk.h"
#include "portfolio.h"
#include "event_bus.h"
#include "matching_engine.h"
#include <stdio.h>

/* Was 16. Each game room consumes 3 account ids (player 1, player 2,
 * market-maker bot) allocated from one global counter across all rooms,
 * so 64 supports ~20 concurrent rooms per process. portfolio_t is small;
 * pipeline_t is heap-allocated by the bridge, so the size is fine. */
#define PIPELINE_MAX_ACCOUNTS  64

/* Lifetime order-attribution capacity. Was 1024, which a real trading
 * session (or a bridged API driving continuous order flow) blows through
 * quickly — once full, id_map_remember() silently stops recording new
 * orders and every later fill/attribution lookup for them fails. Bumped
 * generously; pipeline_t must be heap- or static-allocated, not put on
 * the stack, once this array is this size. */
#define PIPELINE_MAX_ID_MAP    65536

typedef struct {
    /* Wired-together components */
    gateway_t       gateway;
    risk_engine_t   risk;
    me_engine_t     engine;
    event_bus_t     bus;

    /* Multiple accounts indexed by account_id (1-based for demo) */
    portfolio_t     accounts[PIPELINE_MAX_ACCOUNTS];

    /* Sequencer state */
    uint64_t        next_seq_no;

    /* Optional Write-Ahead-Log; if non-NULL, every sequenced event is
     * appended before matching. Enables replay + crash recovery. */
    FILE           *wal;

    /* Map server_order_id → account_id so trade events can be attributed.
     * Tiny linear table for the demo; production = hash. */
    struct {
        uint64_t server_order_id;
        uint32_t account_id;
        ob_side_t side;
        ob_price_t price;
        ob_qty_t  qty;
    } id_map[PIPELINE_MAX_ID_MAP];
    uint32_t        id_map_count;

    /* Telemetry */
    uint64_t        validated;
    uint64_t        risk_rejected;
    uint64_t        sequenced;
    uint64_t        matched;
} pipeline_t;

/* Initialise everything: gateway, risk, engine, bus, accounts. */
void pipeline_init(pipeline_t *pl, const char *symbol);

/* Optional: open a WAL file. Pass NULL path to disable. */
void pipeline_set_wal(pipeline_t *pl, const char *path);

/* Add / configure an account. account_id must be 1..PIPELINE_MAX_ACCOUNTS. */
void pipeline_add_account(pipeline_t *pl,
                          uint32_t    account_id,
                          int64_t     starting_cash,
                          int64_t     max_position,
                          int64_t     max_order_notional);

/* Subscribe to the event bus. Mirrors bus_subscribe. */
int  pipeline_subscribe(pipeline_t   *pl,
                        uint32_t      topic_mask,
                        bus_handler_fn handler,
                        void         *ctx);

/* Single-shot submit: equivalent to gateway_submit() — same ack contract. */
gw_ack_t pipeline_submit(pipeline_t *pl, const gw_raw_msg_t *msg);

/*
 * Drain the gateway ring, run every event through validation → risk →
 * sequencer → matching → trade exec → bus. Returns the number of events
 * processed. Call this in a loop (or on a dedicated thread).
 */
uint32_t pipeline_step(pipeline_t *pl);

/* Print summary statistics for every stage. */
void pipeline_print_stats(const pipeline_t *pl);

/* Lookup an account portfolio (NULL if not registered). */
portfolio_t *pipeline_account(pipeline_t *pl, uint32_t account_id);

#endif /* PIPELINE_H */
