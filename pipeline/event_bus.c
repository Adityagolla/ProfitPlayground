/*
 * event_bus.c — Synchronous in-process pub/sub
 *
 * Tiny implementation: O(N) fan-out across at most BUS_MAX_SUBS subscribers.
 * That's intentional — the bus is meant to be cache-resident.
 */

#include "event_bus.h"
#include <string.h>

/* bus_init — clear the subscriber table and counters. */
void bus_init(event_bus_t *b) { memset(b, 0, sizeof(*b)); }

/* bus_subscribe — append one subscriber. Returns slot index or -1 if full. */
int bus_subscribe(event_bus_t *b,
                  uint32_t     topic_mask,
                  bus_handler_fn handler,
                  void         *ctx)
{
    if (b->sub_count >= BUS_MAX_SUBS) return -1;
    b->subs[b->sub_count] = (bus_sub_t){
        .handler    = handler,
        .ctx        = ctx,
        .topic_mask = topic_mask,
    };
    return (int)b->sub_count++;
}

/* bus_publish — deliver to every subscriber whose mask intersects topic. */
void bus_publish(event_bus_t *b, const bus_msg_t *msg)
{
    b->published++;
    for (uint32_t i = 0; i < b->sub_count; i++) {
        bus_sub_t *s = &b->subs[i];
        if ((s->topic_mask & (uint32_t)msg->topic) == 0) continue;
        s->handler(msg, s->ctx);
        b->delivered++;
    }
}
