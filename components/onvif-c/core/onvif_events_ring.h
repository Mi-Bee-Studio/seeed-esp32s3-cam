/*
 * onvif-c core — lock-free-by-contract motion event ring.
 *
 * The ring itself has no synchronization: the port layer wraps push/pop
 * in its own mutex. The producer contract (onvif_c_motion) requires
 * zero-timeout locking, i.e. dropping on contention.
 */

#ifndef ONVIF_C_EVENTS_RING_H
#define ONVIF_C_EVENTS_RING_H

#include <stdbool.h>
#include <stdint.h>

#define ONVIF_C_EVENT_QUEUE_MAX 12   /* per-subscription depth (drop oldest) */

typedef struct {
    bool    active;
    uint8_t score;
    int64_t utc;          /* unix seconds (int64 for host/target parity) */
} onvif_c_event_t;

typedef struct {
    onvif_c_event_t q[ONVIF_C_EVENT_QUEUE_MAX];
    int      head;        /* oldest slot */
    int      count;
    uint32_t generated;   /* cumulative pushes (diagnostic) */
} onvif_c_event_ring_t;

void onvif_c_ring_reset(onvif_c_event_ring_t *r);
bool onvif_c_ring_push(onvif_c_event_ring_t *r, bool active, uint8_t score,
                       int64_t utc);
/** Pop oldest event into *out; returns true when one was popped. */
bool onvif_c_ring_pop(onvif_c_event_ring_t *r, onvif_c_event_t *out);

#endif /* ONVIF_C_EVENTS_RING_H */
