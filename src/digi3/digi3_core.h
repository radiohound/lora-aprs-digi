/* LoRa APRS digipeater core: the decision and the frame to send, with no Arduino or radio code, so it runs and is tested on a PC too.
 *
 * Frame handled: 3C FF 01 + TNC2 text (standard LoRa APRS), repeated as text with the new path. Anything else is ignored.
 *
 * The path rule is the one the LoRa APRS Pi uses: the first hop not yet used decides.
 *   fill-in: WIDE1-1 -> MYCALL*        MYCALL -> MYCALL*
 *   wide:    also WIDE2-1 -> MYCALL*   WIDE2-2 -> MYCALL*,WIDE2-1
 * A path that breaks the WIDEn-N rule (1 <= N <= n <= 7) is never repeated, nor a packet this station already repeated or sent. */
#ifndef DIGI3_CORE_H
#define DIGI3_CORE_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define D3_MODE_OFF    0      /* repeats nothing (still beacons and answers messages to it) */
#define D3_MODE_FILLIN 1
#define D3_MODE_WIDE   2

#define D3_MAX_FRAME 255
#define D3_MAX_HOPS  8
#define D3_MAX_TEXT  252    /* a 255-byte frame less the 3-byte 3C FF 01 prefix */

/* What to send for one received packet. */
typedef struct {
    uint16_t text_len;
    char     text[D3_MAX_TEXT + 1];  /* the packet to send (TNC2) */
    uint32_t key;                    /* duplicate key: the same for every copy of the packet, whatever its path */
    uint8_t  named;                  /* the hop used was this station's own callsign (the sender asked for this digi by name) */
    uint8_t  used_hops;              /* hops already used in the path as received (for cancel-on-hear) */
} d3_job_t;

/* Why a frame was not repeated (for the debug log and the counters). */
enum { D3_OK = 0, D3_NOT_APRS, D3_BAD_FRAME, D3_OWN, D3_PATH_RULE, D3_ALREADY, D3_NO_HOPS, D3_NOT_SERVED, D3_TOO_LONG, D3_DIGI_OFF, D3_BATTERY, D3_REASONS };
const char *d3_reason(int r);

/* The path to repeat with. path is the TNC2 path ("WIDE1-1,WIDE2-1"), mycall upper case ("K6ATV-11"). Returns D3_OK and fills out, or a reason.
 * named (may be NULL) is set to 1 when the hop used was mycall itself. */
int d3_new_path(const char *path, size_t plen, const char *mycall, int mode, char *out, size_t cap, int *named);

/* For cancel-on-hear: the duplicate key of a frame (the same as d3_decide gives) and how many hops of its path are used. Returns 1 if the frame
 * could be read (plain text frame). */
int d3_identify(const uint8_t *fr, size_t n, uint32_t *key, int *used);

/* The packet (TNC2, path as received) of the last frame d3_decide could read. Empty when it could not (not a plain frame, a damaged frame).
 * For the neighbour list and "moving stations only". */
const char *d3_last_text(void);

/* Decide for one received frame; on D3_OK the job holds what is needed to build the repeat. */
int d3_decide(const uint8_t *fr, size_t n, const char *mycall, int mode, d3_job_t *job);

/* The frame to transmit: 3C FF 01 + the packet text. Returns the length, or 0 if it must not be sent. */
size_t d3_build(const d3_job_t *job, uint8_t *out, size_t cap);

uint32_t d3_fnv1a(uint32_t h, const uint8_t *p, size_t n);

#ifdef __cplusplus
}
#endif
#endif
