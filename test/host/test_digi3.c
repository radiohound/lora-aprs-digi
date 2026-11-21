/* Host test of the LoRa APRS digipeater core: path rule, duplicate key, positions, moving-stations-only decisions, station list.
 *   make -C test/host test */
#include "digi3_core.h"
#include "neighbors.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void plain_cases(void)
{
    struct { const char *in, *want; int mode; } T[] = {
        { "K6ATV-3>APZLPI,WIDE1-1:!3700.16N/12134.97W>204/013", "K6ATV-3>APZLPI,K6ATV-11*:!3700.16N/12134.97W>204/013", D3_MODE_FILLIN },
        { "K6ATV-3>APZLPI,WIDE1-1,WIDE2-1:>hi", "K6ATV-3>APZLPI,K6ATV-11*,WIDE2-1:>hi", D3_MODE_WIDE },
        { "K6ATV-3>APZLPI,K6ATV-10*,WIDE2-1:>hi", "K6ATV-3>APZLPI,K6ATV-10*,K6ATV-11*:>hi", D3_MODE_WIDE },
        { "K6ATV-3>APZLPI,K6ATV-10*,WIDE2-1:>hi", NULL, D3_MODE_FILLIN },                     /* fill-in does not serve WIDE2 */
        { "K6ATV-3>APZLPI,WIDE2-2:>hi", "K6ATV-3>APZLPI,K6ATV-11*,WIDE2-1:>hi", D3_MODE_WIDE },
        { "K6ATV-3>APZLPI,K6ATV-11:>hi", "K6ATV-3>APZLPI,K6ATV-11*:>hi", D3_MODE_FILLIN },          /* addressed to this station */
        { "K6ATV-3>APZLPI,K6ATV-11*,WIDE2-1:>hi", NULL, D3_MODE_WIDE },                         /* already repeated here */
        { "K6ATV-11>APLAB0,WIDE1-1:>me", NULL, D3_MODE_WIDE },                                   /* our own packet */
        { "K6ATV-3>APZLPI,WIDE7-8:>hi", NULL, D3_MODE_WIDE },                                    /* breaks WIDEn-N */
        { "K6ATV-3>APZLPI:>no path", NULL, D3_MODE_WIDE },
        { "K6ATV-3>APZLPI,WIDE3-3:>hi", NULL, D3_MODE_WIDE },                                    /* not served */
        { "garbage without colon", NULL, D3_MODE_WIDE },
    };
    size_t i;
    for (i = 0; i < sizeof T / sizeof T[0]; i++) {
        uint8_t fr[300], out[300]; size_t n = strlen(T[i].in), on = 0; d3_job_t job; int r;
        fr[0] = 0x3C; fr[1] = 0xFF; fr[2] = 0x01; memcpy(fr + 3, T[i].in, n);
        r = d3_decide(fr, n + 3, "K6ATV-11", T[i].mode, &job);
        if (r == D3_OK) on = d3_build(&job, out, sizeof out);
        if (!T[i].want) CHECK(on == 0, "plain: should not repeat %s", T[i].in);
        else CHECK(on == strlen(T[i].want) + 3 && !memcmp(out + 3, T[i].want, on - 3) && out[2] == 0x01, "plain: %s -> %.*s (%s)", T[i].in, (int)(on > 3 ? on - 3 : 0), out + 3, d3_reason(r));
    }
    /* the same packet over two paths gets the same duplicate key */
    {
        uint8_t a[100] = { 0x3C, 0xFF, 0x01 }, b[100] = { 0x3C, 0xFF, 0x01 }; d3_job_t ja, jb;
        const char *pa = "K6ATV-3>APZLPI,WIDE1-1,WIDE2-1:>hi", *pb = "K6ATV-3>APZLPI,K6ATV-10*,WIDE2-1:>hi";
        memcpy(a + 3, pa, strlen(pa)); memcpy(b + 3, pb, strlen(pb));
        CHECK(d3_decide(a, strlen(pa) + 3, "K6ATV-11", D3_MODE_WIDE, &ja) == D3_OK && d3_decide(b, strlen(pb) + 3, "K6ATV-11", D3_MODE_WIDE, &jb) == D3_OK && ja.key == jb.key,
              "duplicate key should not depend on the path");
    }
    /* digipeating off */
    { uint8_t f[60] = { 0x3C, 0xFF, 0x01 }; const char *p = "K6ATV-3>APZLPI,WIDE1-1:>hi"; d3_job_t j; memcpy(f + 3, p, strlen(p));
      CHECK(d3_decide(f, strlen(p) + 3, "K6ATV-11", D3_MODE_OFF, &j) == D3_DIGI_OFF && !strcmp(d3_last_text(), p), "mode off: nothing repeated, the packet still read"); }
    /* anything that is not a 3C FF 01 text frame is ignored (other frame types included) */
    { uint8_t f2[] = { 0x3C, 0xFF, 0x02, 0x10, 0x00 }, f3[] = { 0x3C, 0xFF, 0x03, 0x10, 0x00 }; d3_job_t j;
      CHECK(d3_decide(f2, sizeof f2, "K6ATV-11", D3_MODE_WIDE, &j) == D3_NOT_APRS && d3_decide(f3, sizeof f3, "K6ATV-11", D3_MODE_WIDE, &j) == D3_NOT_APRS, "other frame types ignored");
      CHECK(!d3_last_text()[0], "no text from a frame that is not plain"); }
}

/* Positions: the expected values are those aprslib 0.7 gives for the same packets. */
static void position_cases(void)
{
    struct { const char *pkt; double lat, lon; } T[] = {
        { "K6ATV-3>APZLPI,WIDE1-1:!3700.16N/12134.97W>204/013", 37.002667, -121.582833 },
        { "N0CALL>APRS:!/5L!!<*e7>7P[", 49.5, -72.750004 },                          /* compressed (APRS 1.01 example) */
        { "N0CALL>370PQV,WIDE1-1:`1>}l\"v>/", 37.002667, -121.582833 },               /* Mic-E */
        { "N0CALL>3B0PQV:`1>}l\"v>/", 31.002667, -121.582833 },                      /* Mic-E, custom message digit */
        { "N0CALL>S32U6T:`(_fn\"Oj/", 33.427333, -12.129 },                         /* Mic-E, APRS 1.01 destination */
        { "N0CALL>APRS:@092345z3700.16N/12134.97W_090/000g000", 37.002667, -121.582833 },   /* time stamp first */
        { "N0CALL>APRS:=3700.1 N/12134.9 W-", 37.001667, -121.581667 },               /* ambiguity: the spaces count as 0 */
    };
    size_t i;
    for (i = 0; i < sizeof T / sizeof T[0]; i++) {
        const char *t = T[i].pkt, *gt = strchr(t, '>'), *colon = strchr(t, ':'), *de = gt + 1; int32_t lat, lon;
        while (de < colon && *de != ',') de++;
        CHECK(d3_parse_position(gt + 1, (size_t)(de - gt - 1), colon + 1, strlen(colon + 1), &lat, &lon), "position: %s", t);
        CHECK(fabs(lat / 1e6 - T[i].lat) < 2e-6 && fabs(lon / 1e6 - T[i].lon) < 2e-6, "position %s: %.6f %.6f", t, lat / 1e6, lon / 1e6);
    }
    { int32_t a, b; CHECK(!d3_parse_position("APRS", 4, ">status", 7, &a, &b) && !d3_parse_position("APRS", 4, ":N0CALL   :hi", 13, &a, &b), "no position"); }
    CHECK(d3_distance_m(37000000, -121600000, 37001000, -121600000) == 111, "distance north %u", d3_distance_m(37000000, -121600000, 37001000, -121600000));
}

static int move(d3_neighbors_t *nb, const d3_moving_cfg_t *c, const char *t, int named, uint32_t now)
{
    d3_observe(nb, t, strlen(t), -90, 5, now);
    return d3_moving_decision(nb, c, t, strlen(t), named, now);
}

static void moving_cases(void)
{
    d3_station_t slots[4]; d3_neighbors_t nb; d3_station_t *s;
    d3_moving_cfg_t c = { 100, 0, 1, 0, "APLAIX, APLAB" };
    d3_neighbors_init(&nb, slots, 4);
    CHECK(move(&nb, &c, "W1FIX>APLRG1,WIDE1-1:!3700.00N/12136.00W#", 0, 1000) == D3_MOVE_REPEAT_NEW, "first position: new");
    CHECK(move(&nb, &c, "W1FIX>APLRG1,WIDE1-1:!3700.00N/12136.00W#", 0, 61000) == D3_MOVE_SKIP_STATIONARY, "same place: skipped");
    CHECK(move(&nb, &c, "W1FIX>APLRG1,WIDE1-1:!3700.03N/12136.00W#", 0, 121000) == D3_MOVE_SKIP_STATIONARY, "55 m: still fixed");
    CHECK(move(&nb, &c, "W1FIX>APLRG1,WIDE1-1:!3700.06N/12136.00W#", 0, 181000) == D3_MOVE_REPEAT_MOVED, "110 m from the last repeated: moved");
    CHECK(move(&nb, &c, "W1FIX>APLRG1,W1DIG:!3700.06N/12136.00W#", 1, 241000) == D3_MOVE_REPEAT_NAMED, "asked by name");
    CHECK(move(&nb, &c, "SP0LND-1>APLAIX,WIDE1-1:!3700.06N/12136.00W#", 0, 242000) == D3_MOVE_REPEAT_TOCALL, "tocall list");
    CHECK(move(&nb, &c, "SP0LND-1>APLAB1,WIDE1-1:>hello", 0, 243000) == D3_MOVE_REPEAT_TOCALL, "tocall prefix");
    CHECK(move(&nb, &c, "W1FIX>APLRG1,WIDE1-1::K6ATV-3  :hi{1", 0, 244000) == D3_MOVE_REPEAT_MESSAGE, "message");
    CHECK(move(&nb, &c, "W1FIX>APLRG1,WIDE1-1:>status", 0, 245000) == D3_MOVE_SKIP_OTHER, "status: skipped");
    CHECK(d3_moving_decision(&nb, &c, NULL, 0, 0, 246000) == D3_MOVE_SKIP_UNREADABLE, "unreadable");
    c.stationary_repeat_min = 30;
    CHECK(move(&nb, &c, "W1FIX>APLRG1,WIDE1-1:!3700.06N/12136.00W#", 0, 181000 + 30 * 60000) == D3_MOVE_REPEAT_REFRESH, "refresh after 30 min");
    /* the neighbour side: heard directly, and through a digipeater (the digipeater is the one heard; an alias is not a station) */
    s = d3_neighbor_find(&nb, "W1FIX", 5); CHECK(s && s->has_pos && s->direct_src && s->rf_frames >= 1 && s->rssi10 == -900, "direct station");
    { const char *t = "K6ATV-3>APZLPI,K6ATV-10*,WIDE2-1:!3700.16N/12134.97W>"; d3_observe(&nb, t, strlen(t), -77, 3.5f, 2000000); }
    s = d3_neighbor_find(&nb, "K6ATV-10", 8); CHECK(s && s->rf_frames == 1 && s->snr10 == 35 && !s->has_pos, "the digipeater is the RF neighbour");
    s = d3_neighbor_find(&nb, "k6atv-3", 7); CHECK(s && s->has_pos && !s->rf_ms && !s->direct_src, "the source has a position but was not heard directly");
    d3_observe(&nb, "N0ONE>APRS,WIDE1*:>x", 20, -100, 0, 2000001);
    CHECK(!d3_neighbor_find(&nb, "WIDE1", 5), "an alias is not a station");
    /* a full table drops the station heard longest ago */
    d3_observe(&nb, "NEW1>APRS:>a", 12, -100, 0, 3000000); d3_observe(&nb, "NEW2>APRS:>a", 12, -100, 0, 3000001);
    CHECK(d3_neighbor_find(&nb, "NEW1", 4) && d3_neighbor_find(&nb, "NEW2", 4) && !d3_neighbor_find(&nb, "W1FIX", 5) && d3_neighbor_find(&nb, "N0ONE", 5), "eviction drops the station heard longest ago");
}

static void identify_cases(void)
{
    uint8_t a[100] = { 0x3C, 0xFF, 0x01 }, b[100] = { 0x3C, 0xFF, 0x01 }; d3_job_t j; uint32_t ka, kb; int ua, ub;
    const char *pa = "K6ATV-3>APZLPI,WIDE1-1,WIDE2-1:>hi", *pb = "K6ATV-3>APZLPI,K6ATV-10*,WIDE2-1:>hi";
    memcpy(a + 3, pa, strlen(pa)); memcpy(b + 3, pb, strlen(pb));
    CHECK(d3_identify(a, strlen(pa) + 3, &ka, &ua) && d3_identify(b, strlen(pb) + 3, &kb, &ub) && ka == kb && ua == 0 && ub == 1, "identify: key %u %u used %d %d", ka, kb, ua, ub);
    CHECK(d3_decide(a, strlen(pa) + 3, "K6ATV-11", D3_MODE_WIDE, &j) == D3_OK && j.key == ka && j.used_hops == 0 && !j.named, "decide gives the same key");
    CHECK(!strcmp(d3_last_text(), pa), "last text is the packet as received");
    { const char *pn = "K6ATV-3>APZLPI,K6ATV-11:>hi"; uint8_t f[60] = { 0x3C, 0xFF, 0x01 }; memcpy(f + 3, pn, strlen(pn));
      CHECK(d3_decide(f, strlen(pn) + 3, "K6ATV-11", D3_MODE_FILLIN, &j) == D3_OK && j.named, "named"); }
}

int main(void)
{
    plain_cases();
    position_cases();
    moving_cases();
    identify_cases();
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "OK", checks, fails);
    return fails != 0;
}
