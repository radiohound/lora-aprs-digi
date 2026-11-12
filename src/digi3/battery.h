/* Battery watching for the LoRa APRS digipeater: what the supply voltage means. Plain C, no Arduino or radio code, tested on a PC
 * (test/host/test_battery.c). The firmware measures the chip's own supply (the battery, when it is wired straight to the 3.3 V pin) and
 * feeds the millivolts here.
 *
 *   OK     normal
 *   WARN   getting low: only reported (stats, "battery", "get battery" over the air)
 *   LOW    nothing is repeated or beaconed any more (answers to commands over the air still go out); the radio keeps listening
 *   SLEEP  the radio and the processor sleep; the supply is checked every few seconds
 *
 * Going down: a state is entered after BATT_CONFIRM readings in a row that all point to that same state, so one dip while the radio transmits
 * does not count. Going up: only back to OK, after BATT_CONFIRM readings in a row at or above the resume voltage (higher than the warn voltage:
 * the gap keeps it from flapping on a battery that recovers a little). */
#ifndef D3_BATTERY_H
#define D3_BATTERY_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BATT_CONFIRM 3

typedef enum { D3B_OK = 0, D3B_WARN, D3B_LOW, D3B_SLEEP } d3b_state_t;

typedef struct {
    d3b_state_t state;
    d3b_state_t pending;      /* the state the last readings point to */
    uint8_t     count;        /* how many readings in a row pointed to it */
} d3b_t;

/* The thresholds in millivolts: sleep < low < warn <= resume. */
typedef struct { uint16_t warn_mv, low_mv, sleep_mv, resume_mv; } d3b_limits_t;

void d3b_init(d3b_t *b);

/* One reading. Returns 1 when the state changed (read it from b->state). */
int d3b_update(d3b_t *b, unsigned mv, const d3b_limits_t *lim);

const char *d3b_name(d3b_state_t s);

/* The supply voltage from a reading of the internal reference: vref_cal is the factory value (the reference measured at 3.3 V, 12 bits),
 * raw the reading now. 0 if raw is 0. */
unsigned d3b_vdd_mv(uint32_t vref_cal, uint32_t raw);

#ifdef __cplusplus
}
#endif
#endif
