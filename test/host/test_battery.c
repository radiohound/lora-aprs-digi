/* Host test of the battery states (src/digi3/battery.c). */
#include <stdio.h>
#include <string.h>
#include "battery.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static const d3b_limits_t L = { 3100, 3000, 2800, 3150 };

/* feed n readings of the same voltage; returns how many changed the state */
static int feed(d3b_t *b, unsigned mv, int n) { int c = 0; while (n--) c += d3b_update(b, mv, &L); return c; }

int main(void)
{
    d3b_t b; int i;
    d3b_init(&b);
    CHECK(b.state == D3B_OK);

    /* a full or charging cell, and a normal one: nothing happens */
    CHECK(feed(&b, 3650, 20) == 0 && b.state == D3B_OK);
    CHECK(feed(&b, 3300, 20) == 0 && b.state == D3B_OK);
    CHECK(feed(&b, 3100, 20) == 0 && b.state == D3B_OK);          /* exactly the warn voltage is not below it */

    /* one or two dips (a transmission) are ignored, the third reading in a row counts */
    CHECK(feed(&b, 3050, 2) == 0 && b.state == D3B_OK);
    CHECK(feed(&b, 3300, 1) == 0 && b.state == D3B_OK);           /* back up: the count starts again */
    CHECK(feed(&b, 3050, 2) == 0 && b.state == D3B_OK);
    CHECK(feed(&b, 3050, 1) == 1 && b.state == D3B_WARN);

    /* inside the gap between warn and resume it stays in WARN, no flapping */
    CHECK(feed(&b, 3120, 30) == 0 && b.state == D3B_WARN);
    CHECK(feed(&b, 3149, 30) == 0 && b.state == D3B_WARN);
    CHECK(feed(&b, 3150, 2) == 0 && b.state == D3B_WARN);
    CHECK(feed(&b, 3150, 1) == 1 && b.state == D3B_OK);

    /* down to LOW (not repeating), and back up only at the resume voltage */
    CHECK(feed(&b, 2950, 3) == 1 && b.state == D3B_LOW);          /* from OK straight to LOW when the voltage fell fast */
    CHECK(feed(&b, 3050, 30) == 0 && b.state == D3B_LOW);         /* above low, below resume: stays */
    CHECK(feed(&b, 3149, 30) == 0 && b.state == D3B_LOW);
    CHECK(feed(&b, 3200, 3) == 1 && b.state == D3B_OK);

    /* SLEEP, and out of it */
    CHECK(feed(&b, 2700, 3) == 1 && b.state == D3B_SLEEP);
    CHECK(feed(&b, 2900, 30) == 0 && b.state == D3B_SLEEP);       /* recovering a little is not enough */
    CHECK(feed(&b, 3100, 30) == 0 && b.state == D3B_SLEEP);
    CHECK(feed(&b, 3150, 3) == 1 && b.state == D3B_OK);

    /* a slow fall goes through each state in turn */
    d3b_init(&b);
    CHECK(feed(&b, 3090, 3) == 1 && b.state == D3B_WARN);
    CHECK(feed(&b, 2990, 3) == 1 && b.state == D3B_LOW);
    CHECK(feed(&b, 2790, 3) == 1 && b.state == D3B_SLEEP);
    CHECK(feed(&b, 2800, 30) == 0 && b.state == D3B_SLEEP);       /* exactly the sleep voltage: still sleeping, resume is far above */

    /* WARN or LOW never gets better by a reading that is only above sleep: LOW stays LOW at 2990 even after WARN readings */
    d3b_init(&b); feed(&b, 2950, 3);
    CHECK(b.state == D3B_LOW);
    CHECK(feed(&b, 3090, 30) == 0 && b.state == D3B_LOW);

    /* mixed readings that point to different states do not add up */
    d3b_init(&b);
    for (i = 0; i < 30; i++) d3b_update(&b, (i & 1) ? 3050 : 2950, &L);
    CHECK(b.state == D3B_OK);

    /* a reading of zero (the supply could not be read) is below everything: three in a row put it to sleep, so the caller must not feed it */
    CHECK(d3b_vdd_mv(1500, 0) == 0);

    /* the supply from the reference reading: cal 1500 counts at 3.3 V; reading 1500 is 3.3 V, 1650 is 3.0 V, 1364 is 3.63 V */
    CHECK(d3b_vdd_mv(1500, 1500) == 3300);
    CHECK(d3b_vdd_mv(1500, 1650) == 3000);
    CHECK(d3b_vdd_mv(1500, 1364) == 3629 || d3b_vdd_mv(1500, 1364) == 3630);

    /* the names */
    CHECK(strcmp(d3b_name(D3B_OK), "ok") == 0 && strlen(d3b_name(D3B_SLEEP)) > 0);

    if (fails) { printf("FAILED: %d of %d checks\n", fails, checks); return 1; }
    printf("OK: %d checks, 0 failed\n", checks);
    return 0;
}
