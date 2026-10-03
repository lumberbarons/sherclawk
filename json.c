/* JSON for a bounded classic-Mac REST client: no allocation, no substring
 * extraction, strict syntax/UTF-8, and a 32-level nesting ceiling. */
#include "json.h"
#include <string.h>
#include <stdio.h>
#include <limits.h>

int utf8_emit(unsigned long c, char *out, size_t cap)
{
    int n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    int i;
    if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) || cap < (size_t)n) return -1;
    if (n == 1) { out[0] = (char)c; return 1; }
    for (i = n - 1; i > 0; i--) { out[i] = (char)(0x80 | (c & 63)); c >>= 6; }
    out[0] = (char)((n == 2 ? 0xc0 : n == 3 ? 0xe0 : 0xf0) | c);
    return n;
}

int utf8_next(const char *s, size_t len, size_t *at, unsigned long *cp)
{
    unsigned char c;
    unsigned long v, min;
    size_t i = *at;
    int more;
    if (i >= len) return -1;
    c = (unsigned char)s[i++];
    if (c < 128) { *cp = c; *at = i; return 0; }
    if (c >= 0xc2 && c <= 0xdf) { v = c & 31; more = 1; min = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { v = c & 15; more = 2; min = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { v = c & 7; more = 3; min = 0x10000; }
    else return -1;
    while (more--) {
        if (i >= len || ((unsigned char)s[i] & 0xc0) != 0x80) return -1;
        v = (v << 6) | ((unsigned char)s[i++] & 63);
    }
    if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return -1;
    *cp = v; *at = i; return 0;
}

static int hex4(const char *s, size_t n, size_t *at, unsigned long *v)
{
    int j;
    *v = 0;
    for (j = 0; j < 4; j++) {
        int d;
        if (*at >= n) return -1;
        d = (unsigned char)s[(*at)++];
        if (d >= '0' && d <= '9') d -= '0';
        else if (d >= 'a' && d <= 'f') d -= 'a' - 10;
        else if (d >= 'A' && d <= 'F') d -= 'A' - 10;
        else return -1;
        *v = (*v << 4) | (unsigned)d;
    }
    return 0;
}

/* Decode a quoted string; out==NULL validates without allocating storage. */
static int read_string(const char *s, size_t n, size_t *at, char *out, size_t cap)
{
    size_t used = 0;
    if (*at >= n || s[(*at)++] != '"') return -1;
    while (*at < n) {
        unsigned long c;
        char bytes[4];
        int k;
        if (s[*at] == '"') {
            (*at)++;
            if (out) { if (used >= cap) return -1; out[used] = 0; }
            return (int)used;
        }
        if (s[*at] == '\\') {
            (*at)++;
            if (*at >= n) return -1;
            c = (unsigned char)s[(*at)++];
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = 8; break;
            case 'f': c = 12; break;
            case 'n': c = 10; break;
            case 'r': c = 13; break;
            case 't': c = 9; break;
            case 'u': {
                unsigned long lo;
                if (hex4(s, n, at, &c)) return -1;
                if (c >= 0xd800 && c <= 0xdbff) {
                    if (n - *at < 6 || s[*at] != '\\' || s[*at + 1] != 'u') return -1;
                    *at += 2;
                    if (hex4(s, n, at, &lo) || lo < 0xdc00 || lo > 0xdfff) return -1;
                    c = 0x10000 + ((c - 0xd800) << 10) + lo - 0xdc00;
                } else if (c >= 0xdc00 && c <= 0xdfff) return -1;
                break;
            }
            default: return -1;
            }
        } else {
            if ((unsigned char)s[*at] < 32 || utf8_next(s, n, at, &c)) return -1;
        }
        /* Strings are C strings in this client; embedded NUL is unsupported. */
        if (!c) return -1;
        k = utf8_emit(c, bytes, sizeof(bytes));
        if (k < 0 || (out && (used >= cap || (size_t)k >= cap - used))) return -1;
        if (out) memcpy(out + used, bytes, (size_t)k);
        used += (size_t)k;
    }
    return -1;
}

typedef struct { const char *s; size_t n, at; JsonToken *t; int used, cap; } Reader;
static void ws(Reader *r)
{
    while (r->at < r->n && (r->s[r->at] == ' ' || r->s[r->at] == '\r' ||
           r->s[r->at] == '\n' || r->s[r->at] == '\t')) r->at++;
}
static int value(Reader *r, int depth)
{
    int id, kind;
    char c;
    size_t start;
    ws(r);
    if (depth > 32 || r->at >= r->n || r->used == r->cap) return -1;
    id = r->used++; start = r->at; c = r->s[r->at];
    kind = c == '{' ? JSON_OBJECT : c == '[' ? JSON_ARRAY : c == '"' ? JSON_STRING : JSON_PRIMITIVE;
    r->t[id].type = kind; r->t[id].start = (int)start;
    if (kind == JSON_STRING) {
        if (read_string(r->s, r->n, &r->at, NULL, 0) < 0) return -1;
    } else if (kind == JSON_OBJECT || kind == JSON_ARRAY) {
        char end = kind == JSON_OBJECT ? '}' : ']';
        r->at++; ws(r);
        if (r->at < r->n && r->s[r->at] == end) r->at++;
        else for (;;) {
            if (kind == JSON_OBJECT) {
                int key;
                ws(r);
                if (r->at >= r->n || r->s[r->at] != '"') return -1;
                key = value(r, depth + 1);
                if (key < 0) return -1;
                ws(r);
                if (r->at >= r->n || r->s[r->at++] != ':') return -1;
            }
            if (value(r, depth + 1) < 0) return -1;
            ws(r);
            if (r->at >= r->n) return -1;
            if (r->s[r->at] == end) { r->at++; break; }
            if (r->s[r->at++] != ',') return -1;
        }
    } else if (c == 't' || c == 'f' || c == 'n') {
        const char *word = c == 't' ? "true" : c == 'f' ? "false" : "null";
        size_t k = strlen(word);
        if (r->n - r->at < k || memcmp(r->s + r->at, word, k)) return -1;
        r->at += k;
    } else {
        if (c == '-') r->at++;
        if (r->at >= r->n) return -1;
        if (r->s[r->at] == '0') r->at++;
        else {
            if (r->s[r->at] < '1' || r->s[r->at] > '9') return -1;
            do { r->at++; } while (r->at < r->n && r->s[r->at] >= '0' && r->s[r->at] <= '9');
        }
        if (r->at < r->n && r->s[r->at] == '.') {
            r->at++; start = r->at;
            while (r->at < r->n && r->s[r->at] >= '0' && r->s[r->at] <= '9') r->at++;
            if (r->at == start) return -1;
        }
        if (r->at < r->n && (r->s[r->at] == 'e' || r->s[r->at] == 'E')) {
            r->at++;
            if (r->at < r->n && (r->s[r->at] == '+' || r->s[r->at] == '-')) r->at++;
            start = r->at;
            while (r->at < r->n && r->s[r->at] >= '0' && r->s[r->at] <= '9') r->at++;
            if (r->at == start) return -1;
        }
    }
    r->t[id].end = (int)r->at; r->t[id].next = r->used;
    return id;
}
int json_parse(const char *s, size_t len, JsonToken *tokens, int cap)
{
    Reader r = {s, len, 0, tokens, 0, cap};
    if (len > INT_MAX || value(&r, 0) < 0) return -1;
    ws(&r); return r.at == len ? r.used : -1;
}
int json_string(const char *s, const JsonToken *t, int index, char *out, size_t cap)
{
    size_t at;
    if (index < 0 || t[index].type != JSON_STRING) return -1;
    at = (size_t)t[index].start;
    return read_string(s, (size_t)t[index].end, &at, out, cap);
}
int json_member(const char *s, const JsonToken *t, int object, const char *key)
{
    int i;
    char name[128];
    if (object < 0 || t[object].type != JSON_OBJECT) return -1;
    for (i = object + 1; i < t[object].next;) {
        int v = i + 1;
        if (json_string(s, t, i, name, sizeof(name)) >= 0 && !strcmp(name, key)) return v;
        i = t[v].next;
    }
    return -1;
}
int json_quote(const char *s, char *out, size_t cap)
{
    size_t at = 0, n = strlen(s), used = 1;
    if (cap < 3) return -1;
    out[0] = '"';
    while (at < n) {
        size_t start = at, k;
        unsigned long c;
        char escaped[7];
        const char *bytes = s + start;
        if (utf8_next(s, n, &at, &c)) return -1;
        k = at - start;
        if (c == '"' || c == '\\') { escaped[0] = '\\'; escaped[1] = (char)c; bytes = escaped; k = 2; }
        else if (c < 32) { snprintf(escaped, sizeof(escaped), "\\u%04lx", c); bytes = escaped; k = 6; }
        if (used >= cap || k > cap - used - 1) return -1;
        memcpy(out + used, bytes, k); used += k;
    }
    if (cap - used < 2) return -1;
    out[used++] = '"'; out[used] = 0; return (int)used;
}
