/* Host test of src/digi3/remote.c (settings over the air) and the over-the-air checks in settings.c. With python3 on the PATH it also checks
 * that tools/remote_code.py makes the same codes as the firmware. */
#include "remote.h"
#include "settings.h"
#include "digi3_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static char out[64][200]; static int nout;
static void emit(void *ctx, const char *l) { (void)ctx; if (nout < 64) { strncpy(out[nout], l, 199); out[nout][199] = 0; } nout++; }
static int said(const char *needle) { int i; for (i = 0; i < nout && i < 64; i++) if (strstr(out[i], needle)) return 1; return 0; }

int main(void)
{
    uint8_t key[16], k2[16]; int i; char c1[9], c2[9], text[100], msgno[8], cmd[80], code[12]; uint32_t ctr;

    /* SipHash-2-4: the reference vector of the paper (key 00..0f, message 00..0e) */
    for (i = 0; i < 16; i++) key[i] = (uint8_t)i;
    { uint8_t m[15]; for (i = 0; i < 15; i++) m[i] = (uint8_t)i; CHECK(d3r_siphash(key, m, 15) == 0xa129ca6149be45e5ULL); }
    { uint8_t m[64]; for (i = 0; i < 64; i++) m[i] = (uint8_t)i; CHECK(d3r_siphash(key, m, 0) == 0x726fdb47dd0e0e31ULL && d3r_siphash(key, m, 8) == 0x93f5f5799a932462ULL); }

    /* keys and codes */
    d3r_key_from_uid(0x2F004000, 0x1350524E, 0x34393620, key);
    CHECK(key[0] == 0x00 && key[3] == 0x2F && key[12] == 'L' && key[15] == 'A');
    d3r_code(key, "K6ATV-10", 1794200000u, "set moving on", c1);
    CHECK(strlen(c1) == 8);
    CHECK(d3r_check(key, "K6ATV-10", 1794200000u, "set moving on", c1));
    { char low[9]; for (i = 0; i < 9; i++) low[i] = (char)(c1[i] >= 'A' && c1[i] <= 'Z' ? c1[i] + 32 : c1[i]); CHECK(d3r_check(key, "K6ATV-10", 1794200000u, "set moving on", low)); }
    CHECK(!d3r_check(key, "K6ATV-10", 1794200001u, "set moving on", c1));      /* another counter */
    CHECK(!d3r_check(key, "K6ATV-10", 1794200000u, "set moving off", c1));     /* another command */
    CHECK(!d3r_check(key, "K6ATV-11", 1794200000u, "set moving on", c1));      /* another digi */
    d3r_key_from_secret("my own secret", k2);
    d3r_code(k2, "K6ATV-10", 1794200000u, "set moving on", c2);
    CHECK(strcmp(c1, c2) != 0 && !d3r_check(k2, "K6ATV-10", 1794200000u, "set moving on", c1));

    /* the same codes as the tool */
    if (system("python3 -c 1 2>/dev/null") == 0) {
        FILE *p = popen("python3 ../../tools/remote_code.py K6ATV-10 --chip 2F0040001350524E34393620 --counter 1794200000 set moving on && "
                        "python3 ../../tools/remote_code.py K6ATV-10 --secret 'my own secret' --counter 1794200000 'set   moving on'", "r");
        char a[100] = "", b[100] = "", want[100];
        if (p) { if (!fgets(a, sizeof a, p)) a[0] = 0; if (!fgets(b, sizeof b, p)) b[0] = 0; pclose(p); }
        snprintf(want, sizeof want, "1794200000 set moving on %s\n", c1); CHECK(strcmp(a, want) == 0);
        snprintf(want, sizeof want, "1794200000 set moving on %s\n", c2); CHECK(strcmp(b, want) == 0);
    } else printf("(python3 not found: the tool was not compared)\n");

    /* messages */
    { const char *m = ":K6ATV-10 :1794200000 set moving on ABCDEFGH{17"; CHECK(d3r_message_to(m, strlen(m), "K6ATV-10", text, sizeof text, msgno, sizeof msgno)); }
    CHECK(strcmp(text, "1794200000 set moving on ABCDEFGH") == 0 && strcmp(msgno, "17") == 0);
    CHECK(d3r_message_to(":k6atv-10 :hi", 13, "K6ATV-10", text, sizeof text, msgno, sizeof msgno) && !msgno[0] && !strcmp(text, "hi"));
    CHECK(d3r_message_to(":K6ATV-10 :x{AB}CD", 18, "K6ATV-10", text, sizeof text, msgno, sizeof msgno) && !strcmp(msgno, "AB"));   /* reply-ack form */
    CHECK(!d3r_message_to(":K6ATV-1  :hi", 13, "K6ATV-10", text, sizeof text, msgno, sizeof msgno));
    CHECK(!d3r_message_to(":K6ATV-100:hi", 13, "K6ATV-10", text, sizeof text, msgno, sizeof msgno));
    CHECK(!d3r_message_to("!3700.00N/12100.00W-", 20, "K6ATV-10", text, sizeof text, msgno, sizeof msgno));
    CHECK(d3r_split("1794200000 set comment hello there ABCDEFGH", &ctr, cmd, sizeof cmd, code, sizeof code));
    CHECK(ctr == 1794200000u && !strcmp(cmd, "set comment hello there") && !strcmp(code, "ABCDEFGH"));
    CHECK(!d3r_split("set moving on ABCDEFGH", &ctr, cmd, sizeof cmd, code, sizeof code));      /* no counter */
    CHECK(!d3r_split("1794200000 set moving on ABCDEFG", &ctr, cmd, sizeof cmd, code, sizeof code));   /* short code */
    CHECK(!d3r_split("1794200000 ABCDEFGH", &ctr, cmd, sizeof cmd, code, sizeof code));             /* no command */
    CHECK(!d3r_split("99999999999 save ABCDEFGH", &ctr, cmd, sizeof cmd, code, sizeof code));      /* counter too big */

    /* what may be done over the air */
    CHECK(d3s_remote_check("set moving on") == NULL && d3s_remote_check("get move_m") == NULL && d3s_remote_check("save") == NULL);
    CHECK(d3s_remote_check("beacon") == NULL && d3s_remote_check("reboot force") == NULL && d3s_remote_check("set rc_path WIDE1-1") == NULL);
    CHECK(d3s_remote_check("set call N0CALL") != NULL && d3s_remote_check("set ssid 3") != NULL);
    CHECK(d3s_remote_check("set batt_sleep 2900") != NULL && d3s_remote_check("set batt off") != NULL && d3s_remote_check("get batt_low") == NULL);   /* limits only on the serial port */
    CHECK(d3s_remote_check("set freq 439.9") != NULL && d3s_remote_check("set sf 7") != NULL && d3s_remote_check("set boosted off") != NULL);
    CHECK(d3s_remote_check("set secret x") != NULL && d3s_remote_check("get secret") != NULL && d3s_remote_check("set rc_counter 0") != NULL);
    CHECK(d3s_remote_check("defaults") != NULL && d3s_remote_check("apply") != NULL && d3s_remote_check("show") != NULL);
    CHECK(d3s_remote_check("set nosuch 1") != NULL && d3s_remote_check("") != NULL);

    /* the secret is never shown, and round-trips through the record */
    {
        d3_settings_t s, t; uint8_t rec[D3S_REC_MAX]; size_t n;
        d3s_defaults(&s);
        nout = 0; d3s_exec(&s, "get secret", emit, NULL); CHECK(said("chip ID"));
        nout = 0; d3s_exec(&s, "set secret Hello World 123", emit, NULL); CHECK(!strcmp(s.secret, "Hello World 123") && said("(set)") && !said("Hello"));
        nout = 0; d3s_exec(&s, "show", emit, NULL); CHECK(!said("Hello") && said("rc_path"));
        s.rc_counter = 1794200000u;
        n = d3s_pack(&s, rec, sizeof rec); CHECK(n > 0);
        CHECK(d3s_unpack(rec, n, &t, NULL) > 0 && !strcmp(t.secret, "Hello World 123") && t.rc_counter == 1794200000u && t.remote == s.remote);
        nout = 0; d3s_exec(&s, "set boosted off", emit, NULL); CHECK(said("apply"));      /* boosted is a radio setting now */
        nout = 0; d3s_exec(&s, "defaults", emit, NULL); CHECK(s.rc_counter == 1794200000u);   /* defaults keeps the counter */
    }

    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "OK", checks, fails);
    return fails != 0;
}
