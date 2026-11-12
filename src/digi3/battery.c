/* Battery watching: see battery.h. */
#include "battery.h"

void d3b_init(d3b_t *b) { b->state = D3B_OK; b->pending = D3B_OK; b->count = 0; }

/* The state a reading points to, given the state now (hysteresis: from LOW or WARN only the resume voltage brings it back to OK). */
static d3b_state_t target(d3b_state_t now, unsigned mv, const d3b_limits_t *l)
{
    if (mv < l->sleep_mv) return D3B_SLEEP;
    if (mv < l->low_mv)   return D3B_LOW > now ? D3B_LOW : now;          /* never better than now just because it is above sleep */
    if (mv < l->warn_mv)  return D3B_WARN > now ? D3B_WARN : now;
    if (mv >= l->resume_mv) return D3B_OK;
    return now;                                                          /* between warn and resume: stay where it is */
}

int d3b_update(d3b_t *b, unsigned mv, const d3b_limits_t *lim)
{
    d3b_state_t t = target(b->state, mv, lim);
    if (t == b->state) { b->pending = b->state; b->count = 0; return 0; }
    if (t == b->pending) { if (b->count < 255) b->count++; } else { b->pending = t; b->count = 1; }
    if (b->count < BATT_CONFIRM) return 0;
    b->state = t; b->count = 0;
    return 1;
}

const char *d3b_name(d3b_state_t s)
{
    switch (s) {
    case D3B_OK:    return "ok";
    case D3B_WARN:  return "low (warning)";
    case D3B_LOW:   return "low (not repeating)";
    default:        return "empty (sleeping)";
    }
}

unsigned d3b_vdd_mv(uint32_t vref_cal, uint32_t raw)
{
    if (!raw) return 0;
    return (unsigned)((3300UL * vref_cal + raw / 2) / raw);
}
