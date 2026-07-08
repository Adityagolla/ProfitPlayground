#ifndef EVENT_BUS_H
#define EVENT_BUS_H

/*
 * event_bus.h — In-process pub/sub fan-out
 *
 * The Trade Execution stage publishes to the bus. Subscribers (UI, ML,
 * Analytics, Outbound Gateway) register a callback and a topic mask.
 *
 * Topics are bitflags so a subscriber can listen to multiple at once:
 *
 *   uint32_t topics = BUS_TOPIC_TRADE | BUS_TOPIC_ORDER;
 *   bus_subscribe(&bus, topics, handler, ctx);
 *
 * Synchronous dispatch in this implementation. Production would add a
 * per-subscriber ring + dedicated thread to keep slow subscribers from
 * back-pressuring the matching engine.
 */

#include "matching_engine.h"
#include "gateway.h"

typedef enum {
    BUS_TOPIC_ORDER     = 1 << 0,   /* accepted/rejected/cancelled/modified */
    BUS_TOPIC_TRADE     = 1 << 1,   /* fills                                */
    BUS_TOPIC_BOOK      = 1 << 2,   /* top-of-book updates                  */
    BUS_TOPIC_PORTFOLIO = 1 << 3,   /* P&L / position updates               */
    BUS_TOPIC_GATEWAY   = 1 << 4,   /* admission / rejection at the gateway */
} bus_topic_t;

/* Generic envelope. Subscribers cast `payload` based on `topic`. */
typedef struct {
    bus_topic_t  topic;
    uint64_t     ts_ns;
    uint32_t     account_id;
    const void  *payload;          /* me_event_t* / order_event_t* / etc.  */
} bus_msg_t;

typedef void (*bus_handler_fn)(const bus_msg_t *msg, void *ctx);

#define BUS_MAX_SUBS  16

typedef struct {
    bus_handler_fn  handler;
    void           *ctx;
    uint32_t        topic_mask;
} bus_sub_t;

typedef struct {
    bus_sub_t  subs[BUS_MAX_SUBS];
    uint32_t   sub_count;
    uint64_t   published;
    uint64_t   delivered;
} event_bus_t;

/* Zero the bus. Must be called before subscribe / publish. */
void bus_init(event_bus_t *b);

/* Register a callback. Returns -1 if the bus is full. */
int  bus_subscribe(event_bus_t *b,
                   uint32_t     topic_mask,
                   bus_handler_fn handler,
                   void         *ctx);

/* Publish a message. Synchronously calls every matching subscriber. */
void bus_publish(event_bus_t *b, const bus_msg_t *msg);

#endif /* EVENT_BUS_H */
