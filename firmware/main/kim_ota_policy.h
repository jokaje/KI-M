#pragma once
/* Exact, ASCII-only wire format. The bytes preceding signature= are signed. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

typedef struct {
    char version[32];
    char image[96];
    uint8_t sha256[32];
    size_t size;
    size_t signed_len;
    uint8_t signature[80]; /* DER ECDSA P-256 */
    size_t signature_len;
} kim_ota_manifest_t;

static inline int kim_ota_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static inline bool kim_ota_dec(const char *s, size_t n, unsigned *out) {
    if (!n || (n > 1 && *s == '0')) return false;
    unsigned v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9' || v > (UINT_MAX - (unsigned)(s[i]-'0')) / 10) return false;
        v = v * 10 + (unsigned)(s[i]-'0');
    }
    *out = v;
    return true;
}
static inline bool kim_ota_semver(const char *s, unsigned v[3]) {
    for (int i = 0; i < 3; i++) {
        const char *start = s;
        while (*s >= '0' && *s <= '9') s++;
        if (!kim_ota_dec(start, (size_t)(s-start), &v[i])) return false;
        if (i != 2) { if (*s++ != '.') return false; }
    }
    return !*s;
}
/* -2 malformed, otherwise numeric comparison. */
static inline int kim_ota_version_compare(const char *a, const char *b) {
    unsigned x[3], y[3];
    if (!kim_ota_semver(a, x) || !kim_ota_semver(b, y)) return -2;
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] > y[i] ? 1 : -1;
    return 0;
}
/* Only exact https://host[:port]/... origins without userinfo, query or fragments. */
static inline size_t kim_ota_origin_len(const char *s) {
    if (!s || strncmp(s, "https://", 8)) return 0;
    const char *p = s + 8;
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) return 0;
    for (; *p && *p != '/'; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '-' || *p == ':')) return 0;
    }
    return *p == '/' ? (size_t)(p-s) : 0;
}
static inline bool kim_ota_same_origin(const char *a, const char *b) {
    size_t x = kim_ota_origin_len(a), y = kim_ota_origin_len(b);
    return x && x == y && memcmp(a,b,x) == 0;
}
static inline bool kim_ota_image_name(const char *s) {
    size_t n = strlen(s);
    if (!n || n >= 96 || s[0] == '.') return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_' || s[i] == '.')) return false;
    return n >= 5 && strcmp(s+n-4, ".bin") == 0 && !strstr(s, "..");
}
static inline bool kim_ota_field(const char *s, size_t n, size_t *pos, const char *key,
                                  char *out, size_t cap) {
    size_t k = strlen(key), start = *pos;
    if (start + k >= n || memcmp(s+start,key,k)) return false;
    start += k;
    size_t end = start;
    while (end < n && s[end] != '\n') end++;
    if (end == n || end == start || end-start >= cap) return false;
    memcpy(out,s+start,end-start); out[end-start] = 0;
    *pos = end+1;
    return true;
}
static inline bool kim_ota_parse(const char *s, size_t n, kim_ota_manifest_t *m) {
    if (!s || !m || n > 512 || n < 120 || memcmp(s,"KIM-OTA-v1\n",11)) return false;
    memset(m,0,sizeof(*m));
    size_t p = 11;
    char model[32], digits[16], hash[65], sig[161];
    unsigned size;
    if (!kim_ota_field(s,n,&p,"version=",m->version,sizeof(m->version)) ||
        !kim_ota_field(s,n,&p,"model=",model,sizeof(model)) ||
        !kim_ota_field(s,n,&p,"size=",digits,sizeof(digits)) ||
        !kim_ota_field(s,n,&p,"sha256=",hash,sizeof(hash)) ||
        !kim_ota_field(s,n,&p,"image=",m->image,sizeof(m->image))) return false;
    unsigned v[3];
    if (!kim_ota_semver(m->version,v) || strcmp(model,"ki-m-box3") ||
        !kim_ota_dec(digits,strlen(digits),&size) || size < 1024 ||
        strlen(hash) != 64 || !kim_ota_image_name(m->image)) return false;
    m->size = size;
    for (size_t i = 0; i < 32; i++) {
        int a = kim_ota_hex(hash[2*i]), b = kim_ota_hex(hash[2*i+1]);
        if (a < 0 || b < 0) return false;
        m->sha256[i] = (uint8_t)((a<<4)|b);
    }
    m->signed_len = p;
    if (!kim_ota_field(s,n,&p,"signature=",sig,sizeof(sig)) || p != n) return false;
    size_t slen = strlen(sig);
    if (slen < 16 || slen > 160 || (slen & 1)) return false;
    m->signature_len = slen/2;
    for (size_t i = 0; i < m->signature_len; i++) {
        int a = kim_ota_hex(sig[2*i]), b = kim_ota_hex(sig[2*i+1]);
        if (a < 0 || b < 0) return false;
        m->signature[i] = (uint8_t)((a<<4)|b);
    }
    return true;
}
