/*
 * onvif-c core — motion event ring (fixed-depth, drop-oldest).
 */

#include "onvif_events_ring.h"

void onvif_c_ring_reset(onvif_c_event_ring_t *r)
{
    r->head      = 0;
    r->count     = 0;
    r->generated = 0;
}

bool onvif_c_ring_push(onvif_c_event_ring_t *r, bool active, uint8_t score, int64_t utc)
{
    if (r->count == ONVIF_C_EVENT_QUEUE_MAX) {
        r->head = (r->head + 1) % ONVIF_C_EVENT_QUEUE_MAX;
        r->count--;
    }
    onvif_c_event_t *e = &r->q[(r->head + r->count) % ONVIF_C_EVENT_QUEUE_MAX];
    e->active          = active;
    e->score           = score;
    e->utc             = utc;
    r->count++;
    r->generated++;
    return true;
}

bool onvif_c_ring_pop(onvif_c_event_ring_t *r, onvif_c_event_t *out)
{
    if (r->count == 0) {
        return false;
    }
    *out    = r->q[r->head];
    r->head = (r->head + 1) % ONVIF_C_EVENT_QUEUE_MAX;
    r->count--;
    return true;
}
