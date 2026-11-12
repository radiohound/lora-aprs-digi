/* LoRa APRS digipeater core. See digi3_core.h. */
#include "digi3_core.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>
static char last_text[D3_MAX_TEXT + 1];
const char *d3_last_text(void) { return last_text; }
static void keep_text(const char *t, size_t n) { if (n > D3_MAX_TEXT) n = D3_MAX_TEXT; memcpy(last_text, t, n); last_text[n] = 0; }

static const char *const REASONS[D3_REASONS] = {
    "ok", "not a LoRa APRS text frame", "damaged or invalid frame", "our own packet",
    "path breaks the WIDEn-N rule", "already repeated by this station", "no hops left", "not a hop this digipeater serves", "too long", "digipeating is off", "battery low"
};

const char *d3_reason(int r) { return (r >= 0 && r < D3_REASONS) ? REASONS[r] : "?"; }

uint32_t d3_fnv1a(uint32_t h, const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static int ieq(const char *a, size_t an, const char *b)
{
    size_t i;
    if (an != strlen(b)) return 0;
    for (i = 0; i < an; i++) if (toupper((unsigned char)a[i]) != toupper((unsigned char)b[i])) return 0;
    return 1;
}

/* WIDEn-N must have 1 <= N <= n <= 7; other hops pass. */
static int wide_ok(const char *h, size_t l)
{
    int n, N = 0; size_t i;
    if (l && h[l - 1] == '*') l--;
    if (l < 7 || toupper((unsigned char)h[0]) != 'W' || toupper((unsigned char)h[1]) != 'I' || toupper((unsigned char)h[2]) != 'D' ||
        toupper((unsigned char)h[3]) != 'E' || !isdigit((unsigned char)h[4]) || h[5] != '-') return 1;
    for (i = 6; i < l; i++) { if (!isdigit((unsigned char)h[i])) return 1; N = N * 10 + (h[i] - '0'); if (N > 99) return 0; }
    n = h[4] - '0';
    return N >= 1 && N <= n && n <= 7;
}

int d3_new_path(const char *path, size_t plen, const char *mycall, int mode, char *out, size_t cap, int *named)
{
    const char *hop[D3_MAX_HOPS + 1]; size_t hl[D3_MAX_HOPS + 1]; int nh = 0, used = -1, k, i;
    size_t s = 0, e, w = 0;
    char mystar[24];
    if (named) *named = 0;
    if (mode == D3_MODE_OFF) return D3_DIGI_OFF;
    if (mode != D3_MODE_FILLIN && mode != D3_MODE_WIDE) return D3_NOT_SERVED;
    while (s < plen) {                                                   /* split on commas, empty hops skipped */
        e = s; while (e < plen && path[e] != ',') e++;
        if (e > s) { if (nh == D3_MAX_HOPS) return D3_TOO_LONG; hop[nh] = path + s; hl[nh] = e - s; nh++; }
        s = e + 1;
    }
    for (i = 0; i < nh; i++) if (!wide_ok(hop[i], hl[i])) return D3_PATH_RULE;
    for (i = 0; i < nh; i++) if (hl[i] && hop[i][hl[i] - 1] == '*') used = i;
    for (i = 0; i <= used; i++) { size_t l = hl[i]; if (l && hop[i][l - 1] == '*') l--; if (ieq(hop[i], l, mycall)) return D3_ALREADY; }
    k = used + 1;
    if (k >= nh) return D3_NO_HOPS;
    snprintf(mystar, sizeof mystar, "%s*", mycall);
    {
        const char *repl[2]; int nr;
        if (ieq(hop[k], hl[k], mycall)) { repl[0] = mystar; nr = 1; if (named) *named = 1; }
        else if (ieq(hop[k], hl[k], "WIDE1-1")) { repl[0] = mystar; nr = 1; }
        else if (mode == D3_MODE_WIDE && ieq(hop[k], hl[k], "WIDE2-1")) { repl[0] = mystar; nr = 1; }
        else if (mode == D3_MODE_WIDE && ieq(hop[k], hl[k], "WIDE2-2")) { repl[0] = mystar; repl[1] = "WIDE2-1"; nr = 2; }
        else return D3_NOT_SERVED;
        if (nh - 1 + nr > D3_MAX_HOPS) return D3_TOO_LONG;
        for (i = 0; i < nh; i++) {
            const char *p; size_t l; int r, reps = (i == k) ? nr : 1;
            for (r = 0; r < reps; r++) {
                if (i == k) { p = repl[r]; l = strlen(p); } else { p = hop[i]; l = hl[i]; }
                if (w + l + 2 > cap) return D3_TOO_LONG;
                if (w) out[w++] = ',';
                memcpy(out + w, p, l); w += l;
            }
        }
    }
    out[w] = 0;
    return D3_OK;
}

/* "src>dest,path:" -> its parts. info may be empty. */
static int split_tnc2(const char *t, size_t n, size_t *gt, size_t *comma_or_colon, size_t *colon)
{
    size_t i; *gt = *colon = (size_t)-1; *comma_or_colon = (size_t)-1;
    for (i = 0; i < n; i++) if (t[i] == ':') { *colon = i; break; }
    if (*colon == (size_t)-1) return 0;
    for (i = 0; i < *colon; i++) if (t[i] == '>') { *gt = i; break; }
    if (*gt == (size_t)-1 || *gt == 0) return 0;
    for (i = *gt + 1; i < *colon; i++) if (t[i] == ',') break;
    *comma_or_colon = i;                                                 /* end of the destination: a comma before the path, or the colon */
    return *comma_or_colon > *gt + 1;
}

static int used_hops(const char *path, size_t plen)
{
    size_t i; int u = 0, k = 0;
    for (i = 0; i < plen; i++) if (path[i] == '*') u = k + 1; else if (path[i] == ',') k++;
    return u;                                                            /* the index after the last used hop = the number of hops used */
}

static uint32_t text_key(const char *t, size_t n, size_t de, size_t colon)
{
    uint32_t h = d3_fnv1a(2166136261u, (const uint8_t *)t, de);
    return d3_fnv1a(h, (const uint8_t *)t + colon, n - colon);
}

static int plain_len(const uint8_t *fr, size_t n, size_t *len)       /* the text of a 3C FF 01 frame, up to a CR or LF; 0 if it holds a NUL */
{
    size_t i;
    for (i = 3; i < n; i++) if (fr[i] == 0 || fr[i] == '\r' || fr[i] == '\n') { if (fr[i] == 0) return 0; n = i; break; }
    *len = n - 3;
    return 1;
}

int d3_identify(const uint8_t *fr, size_t n, uint32_t *key, int *used)
{
    size_t gt, de, colon, tl;
    if (n < 4 || fr[0] != 0x3C || fr[1] != 0xFF) return 0;
    if (fr[2] == 0x01) {
        const char *t = (const char *)fr + 3;
        if (!plain_len(fr, n, &tl) || !split_tnc2(t, tl, &gt, &de, &colon)) return 0;
        *key = text_key(t, tl, de, colon);
        *used = de < colon ? used_hops(t + de + 1, colon - de - 1) : 0;
        return 1;
    }
    return 0;
}

static int decide_text(const char *t, size_t n, const char *mycall, int mode, char *out, size_t cap, size_t *outlen, uint32_t *key, d3_job_t *job)
{
    size_t gt, de, colon, w; char np[200]; int r, named;
    if (!split_tnc2(t, n, &gt, &de, &colon)) return D3_NOT_APRS;
    if (ieq(t, gt, mycall)) return D3_OWN;
    if (de == colon) return D3_NO_HOPS;                                  /* no path at all */
    r = d3_new_path(t + de + 1, colon - de - 1, mycall, mode, np, sizeof np, &named);
    if (r != D3_OK) return r;
    w = de + 1 + strlen(np) + (n - colon);
    if (w + 1 > cap || w + 3 > D3_MAX_FRAME) return D3_TOO_LONG;   /* +3: the 3C FF 01 prefix on the air */
    memcpy(out, t, de); out[de] = ','; memcpy(out + de + 1, np, strlen(np)); memcpy(out + de + 1 + strlen(np), t + colon, n - colon); out[w] = 0;
    *outlen = w;
    if (key) *key = text_key(t, n, de, colon);
    job->named = (uint8_t)named; job->used_hops = (uint8_t)used_hops(t + de + 1, colon - de - 1);
    return D3_OK;
}

int d3_decide(const uint8_t *fr, size_t n, const char *mycall, int mode, d3_job_t *job)
{
    size_t outlen, tl; int r;
    last_text[0] = 0;
    if (n < 4 || fr[0] != 0x3C || fr[1] != 0xFF || fr[2] != 0x01) return D3_NOT_APRS;   /* plain LoRa APRS only: 3C FF 01 + TNC2 text */
    if (!plain_len(fr, n, &tl)) return D3_BAD_FRAME;
    keep_text((const char *)fr + 3, tl);
    r = decide_text((const char *)fr + 3, tl, mycall, mode, job->text, sizeof job->text, &outlen, &job->key, job);
    if (r != D3_OK) return r;
    job->text_len = (uint16_t)outlen;
    return D3_OK;
}

size_t d3_build(const d3_job_t *job, uint8_t *out, size_t cap)
{
    if (job->text_len + 3u > cap || job->text_len + 3u > D3_MAX_FRAME) return 0;
    out[0] = 0x3C; out[1] = 0xFF; out[2] = 0x01; memcpy(out + 3, job->text, job->text_len);
    return job->text_len + 3u;
}
