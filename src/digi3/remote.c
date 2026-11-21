/* Settings over the air: authentication and message parsing. See remote.h. */
#include "remote.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ---- SipHash-2-4 (Aumasson and Bernstein), 64-bit output. The rounds are a function, not a macro: a quarter of the flash. ---- */
static uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

static void rounds(uint64_t v[4], int n)
{
    while (n--) {
        v[0] += v[1]; v[1] = rotl(v[1], 13); v[1] ^= v[0]; v[0] = rotl(v[0], 32);
        v[2] += v[3]; v[3] = rotl(v[3], 16); v[3] ^= v[2];
        v[0] += v[3]; v[3] = rotl(v[3], 21); v[3] ^= v[0];
        v[2] += v[1]; v[1] = rotl(v[1], 17); v[1] ^= v[2]; v[2] = rotl(v[2], 32);
    }
}

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0; int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void absorb(uint64_t v[4], uint64_t m) { v[3] ^= m; rounds(v, 2); v[0] ^= m; }

uint64_t d3r_siphash(const uint8_t key[16], const uint8_t *m, size_t len)
{
    uint64_t k0 = le64(key), k1 = le64(key + 8), b = (uint64_t)len << 56, v[4];
    size_t i, full = len & ~(size_t)7;
    v[0] = 0x736f6d6570736575ULL ^ k0; v[1] = 0x646f72616e646f6dULL ^ k1; v[2] = 0x6c7967656e657261ULL ^ k0; v[3] = 0x7465646279746573ULL ^ k1;
    for (i = 0; i < full; i += 8) absorb(v, le64(m + i));
    for (i = 0; i < (len & 7); i++) b |= (uint64_t)m[full + i] << (8 * i);
    absorb(v, b);
    v[2] ^= 0xff; rounds(v, 4);
    return v[0] ^ v[1] ^ v[2] ^ v[3];
}

/* ---- keys ---- */
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

void d3r_key_from_uid(uint32_t w0, uint32_t w1, uint32_t w2, uint8_t key[16])
{
    put32(key, w0); put32(key + 4, w1); put32(key + 8, w2);
    memcpy(key + 12, "LORA", 4);
}

void d3r_key_from_secret(const char *secret, uint8_t key[16])
{
    static const uint8_t KA[16] = { 'L', 'o', 'R', 'a', '-', 'd', 'i', 'g', 'i', '-', 'k', 'e', 'y', '-', 'A', '!' };
    static const uint8_t KB[16] = { 'L', 'o', 'R', 'a', '-', 'd', 'i', 'g', 'i', '-', 'k', 'e', 'y', '-', 'B', '!' };
    size_t n = strlen(secret);
    put64(key, d3r_siphash(KA, (const uint8_t *)secret, n));
    put64(key + 8, d3r_siphash(KB, (const uint8_t *)secret, n));
}

/* ---- codes ---- */
static const char B32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

void d3r_code(const uint8_t key[16], const char *mycall, uint32_t counter, const char *command, char *out)
{
    char m[160]; int n, i; uint64_t h;
    n = snprintf(m, sizeof m, "%s|%lu|%s", mycall, (unsigned long)counter, command);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof m) n = (int)sizeof m - 1;
    h = d3r_siphash(key, (const uint8_t *)m, (size_t)n) & 0xFFFFFFFFFFULL;            /* 40 bits */
    for (i = D3R_CODE_LEN - 1; i >= 0; i--) { out[i] = B32[h & 31]; h >>= 5; }
    out[D3R_CODE_LEN] = 0;
}

int d3r_check(const uint8_t key[16], const char *mycall, uint32_t counter, const char *command, const char *code)
{
    char want[D3R_CODE_LEN + 1]; int i, diff = 0;
    if (strlen(code) != D3R_CODE_LEN) return 0;
    d3r_code(key, mycall, counter, command, want);
    for (i = 0; i < D3R_CODE_LEN; i++) diff |= want[i] ^ toupper((unsigned char)code[i]);       /* the same time whatever matches */
    return diff == 0;
}

/* ---- messages ---- */
const char *d3r_unwrap(const char *packet)
{
    const char *colon = strchr(packet, ':');
    return (colon && colon[1] == '}') ? colon + 2 : packet;
}

int d3r_message_to(const char *info, size_t ilen, const char *mycall, char *text, size_t tcap, char *msgno, size_t mcap)
{
    size_t al, i, end, brace;
    if (ilen < 11 || info[0] != ':' || info[10] != ':') return 0;               /* :ADDRESSEE: with the addressee padded to 9 */
    for (al = 9; al > 0 && info[al] == ' '; al--) {}
    if (al != strlen(mycall)) return 0;
    for (i = 0; i < al; i++) if (toupper((unsigned char)info[1 + i]) != toupper((unsigned char)mycall[i])) return 0;
    end = ilen;
    while (end > 11 && (info[end - 1] == '\r' || info[end - 1] == '\n')) end--;
    for (brace = 11; brace < end && info[brace] != '{'; brace++) {}
    if (brace - 11 >= tcap) return 0;
    memcpy(text, info + 11, brace - 11); text[brace - 11] = 0;
    msgno[0] = 0;
    if (brace < end) {
        size_t ml = end - brace - 1;
        for (i = brace + 1; i < end; i++) if (info[i] == '}') { ml = i - brace - 1; break; }   /* {MM}AA replies: the number before } */
        if (ml && ml < mcap && ml <= 5) { memcpy(msgno, info + brace + 1, ml); msgno[ml] = 0; }
    }
    return 1;
}

int d3r_split(const char *text, uint32_t *counter, char *command, size_t ccap, char *code, size_t kcap)
{
    const char *p = text, *cs, *ce, *ks, *ke = text + strlen(text);
    uint64_t c = 0; int digits = 0;
    while (*p == ' ') p++;
    for (; isdigit((unsigned char)*p); p++) { c = c * 10 + (uint64_t)(*p - '0'); if (++digits > 10 || c > 0xFFFFFFFFULL) return 0; }
    if (!digits || *p != ' ') return 0;
    while (ke > p && ke[-1] == ' ') ke--;
    for (ks = ke; ks > p && ks[-1] != ' '; ks--) {}
    if ((size_t)(ke - ks) != D3R_CODE_LEN || (size_t)(ke - ks) >= kcap) return 0;
    for (cs = p; *cs == ' '; cs++) {}
    for (ce = ks; ce > cs && ce[-1] == ' '; ce--) {}
    if (ce <= cs || (size_t)(ce - cs) >= ccap) return 0;
    memcpy(command, cs, (size_t)(ce - cs)); command[ce - cs] = 0;
    memcpy(code, ks, (size_t)(ke - ks)); code[ke - ks] = 0;
    *counter = (uint32_t)c;
    return 1;
}
