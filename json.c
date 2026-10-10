/* JSON for a bounded classic-Mac REST client: no allocation, no substring
 * extraction, strict syntax/UTF-8, and a 32-level nesting ceiling. */
#include "json.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
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

/* One character of a string body, starting after the opening quote: 1 once
 * the closing quote is consumed, 0 with the decoded code point in *cp (at
 * most 12 input bytes), -1 when invalid. Strings are C strings in this client,
 * so an embedded NUL is unsupported. */
static int string_unit(const char *s, size_t n, size_t *at, unsigned long *cp)
{
    unsigned long c;
    if (*at >= n) return -1;
    if (s[*at] == '"') { (*at)++; return 1; }
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
    } else if ((unsigned char)s[*at] < 32 || utf8_next(s, n, at, &c)) return -1;
    if (!c) return -1;
    *cp = c;
    return 0;
}

/* Decode a quoted string; out==NULL validates without allocating storage. */
static int read_string(const char *s, size_t n, size_t *at, char *out, size_t cap)
{
    size_t used = 0;
    if (*at >= n || s[(*at)++] != '"') return -1;
    for (;;) {
        unsigned long c;
        char bytes[4];
        int k, r = string_unit(s, n, at, &c);
        if (r < 0) return -1;
        if (r) {
            if (out) { if (used >= cap) return -1; out[used] = 0; }
            return (int)used;
        }
        k = utf8_emit(c, bytes, sizeof(bytes));
        if (k < 0 || (out && (used >= cap || (size_t)k >= cap - used))) return -1;
        if (out) memcpy(out + used, bytes, (size_t)k);
        used += (size_t)k;
    }
}

#define JSON_UNLIMITED ((size_t)-1)
enum { P_VALUE, P_STRING, P_NUMBER, P_FIRST, P_KEY, P_COLON, P_AFTER, P_TRAIL, P_DONE, P_FAIL };
enum { N_BEGIN, N_INT, N_INT_DIGITS, N_FRAC, N_FRAC_FIRST, N_FRAC_DIGITS,
       N_EXP, N_EXP_SIGN, N_EXP_FIRST, N_EXP_DIGITS };

void json_parser_init(JsonParser *p, const char *s, size_t len, JsonToken *tokens, int cap)
{
    memset(p, 0, sizeof(*p));
    p->s = s; p->n = len; p->t = tokens; p->cap = cap;
    p->state = len > INT_MAX ? P_FAIL : P_VALUE;
}
static int is_space(int c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static void value_done(JsonParser *p)
{
    p->state = p->depth ? P_AFTER : P_TRAIL;
}
static void scalar_done(JsonParser *p)
{
    p->t[p->current].end = (int)p->at;
    p->t[p->current].next = p->used;
    value_done(p);
}
static void close_container(JsonParser *p)
{
    int id = p->open[--p->depth];
    p->t[id].end = (int)p->at;
    p->t[id].next = p->used;
    value_done(p);
}
/* Every value and key starts here, which is where the nesting ceiling and the
 * token capacity are enforced. */
static int new_token(JsonParser *p, int type)
{
    if (p->depth > JSON_DEPTH || p->used == p->cap) { p->state = P_FAIL; return -1; }
    p->current = p->used++;
    p->t[p->current].type = type;
    p->t[p->current].start = (int)p->at;
    return 0;
}
static void value_start(JsonParser *p, int c)
{
    int kind = c == '{' ? JSON_OBJECT : c == '[' ? JSON_ARRAY : c == '"' ? JSON_STRING : JSON_PRIMITIVE;
    if (new_token(p, kind)) return;
    if (kind == JSON_OBJECT || kind == JSON_ARRAY) {
        p->at++;
        p->open[p->depth++] = p->current;
        p->state = P_FIRST;
    } else if (kind == JSON_STRING) {
        p->at++; p->sub = 0; p->state = P_STRING;
    } else if (c == 't' || c == 'f' || c == 'n') {
        const char *word = c == 't' ? "true" : c == 'f' ? "false" : "null";
        size_t k = strlen(word);
        if (p->n - p->at < k || memcmp(p->s + p->at, word, k)) p->state = P_FAIL;
        else { p->at += k; scalar_done(p); }
    } else {
        p->state = P_NUMBER; p->sub = N_BEGIN;
    }
}
/* One character of a number; a zero-width step moves between sections. */
static void number_step(JsonParser *p)
{
    int c = p->at < p->n ? (unsigned char)p->s[p->at] : -1;
    switch (p->sub) {
    case N_BEGIN:
        if (c == '-') p->at++;
        p->sub = N_INT;
        break;
    case N_INT:
        if (c == '0') { p->at++; p->sub = N_FRAC; }
        else if (c >= '1' && c <= '9') { p->at++; p->sub = N_INT_DIGITS; }
        else p->state = P_FAIL;
        break;
    case N_INT_DIGITS:
        if (is_digit(c)) p->at++; else p->sub = N_FRAC;
        break;
    case N_FRAC:
        if (c == '.') { p->at++; p->sub = N_FRAC_FIRST; } else p->sub = N_EXP;
        break;
    case N_FRAC_FIRST:
        if (is_digit(c)) { p->at++; p->sub = N_FRAC_DIGITS; } else p->state = P_FAIL;
        break;
    case N_FRAC_DIGITS:
        if (is_digit(c)) p->at++; else p->sub = N_EXP;
        break;
    case N_EXP:
        if (c == 'e' || c == 'E') { p->at++; p->sub = N_EXP_SIGN; } else scalar_done(p);
        break;
    case N_EXP_SIGN:
        if (c == '+' || c == '-') p->at++;
        p->sub = N_EXP_FIRST;
        break;
    case N_EXP_FIRST:
        if (is_digit(c)) { p->at++; p->sub = N_EXP_DIGITS; } else p->state = P_FAIL;
        break;
    default:
        if (is_digit(c)) p->at++; else scalar_done(p);
    }
}
/* One atom: at most 12 input bytes (an escaped surrogate pair), or a
 * zero-width move to the next state. */
static void parser_advance(JsonParser *p)
{
    int c, object;
    unsigned long cp;
    if (p->state == P_STRING) {
        int r = string_unit(p->s, p->n, &p->at, &cp);
        if (r < 0) { p->state = P_FAIL; return; }
        if (!r) return;
        p->t[p->current].end = (int)p->at;
        p->t[p->current].next = p->used;
        if (p->sub) p->state = P_COLON; else value_done(p);
        return;
    }
    if (p->state == P_NUMBER) { number_step(p); return; }
    if (p->state == P_TRAIL) {
        if (p->at >= p->n) p->state = P_DONE;
        else if (is_space(p->s[p->at])) p->at++;
        else p->state = P_FAIL;
        return;
    }
    if (p->at >= p->n) { p->state = P_FAIL; return; }
    c = (unsigned char)p->s[p->at];
    if (is_space(c)) { p->at++; return; }
    object = p->depth && p->t[p->open[p->depth - 1]].type == JSON_OBJECT;
    switch (p->state) {
    case P_FIRST:
        if (c == (object ? '}' : ']')) { p->at++; close_container(p); }
        else p->state = object ? P_KEY : P_VALUE;
        break;
    case P_KEY:
        if (c != '"' || new_token(p, JSON_STRING)) { p->state = P_FAIL; break; }
        p->at++; p->sub = 1; p->state = P_STRING;
        break;
    case P_COLON:
        if (c != ':') p->state = P_FAIL;
        else { p->at++; p->state = P_VALUE; }
        break;
    case P_AFTER:
        if (c == (object ? '}' : ']')) { p->at++; close_container(p); }
        else if (c == ',') { p->at++; p->state = object ? P_KEY : P_VALUE; }
        else p->state = P_FAIL;
        break;
    default:
        value_start(p, c);
    }
}
int json_parser_step(JsonParser *p, size_t budget)
{
    size_t from = p->at;
    while (p->state != P_DONE && p->state != P_FAIL) {
        if (p->at != from && p->at - from + JSON_ATOM_MAX > budget) break;
        parser_advance(p);
    }
    p->work = p->at - from;
    return p->state == P_DONE ? 1 : p->state == P_FAIL ? -1 : 0;
}
int json_parse(const char *s, size_t len, JsonToken *tokens, int cap)
{
    JsonParser p;
    json_parser_init(&p, s, len, tokens, cap);
    return json_parser_step(&p, JSON_UNLIMITED) == 1 ? p.used : -1;
}
int json_string(const char *s, const JsonToken *t, int index, char *out, size_t cap)
{
    size_t at;
    if (index < 0 || t[index].type != JSON_STRING) return -1;
    at = (size_t)t[index].start;
    return read_string(s, (size_t)t[index].end, &at, out, cap);
}

static int cursor_next(JsonCursor *c, unsigned long *cp)
{
    return string_unit(c->s, c->end, &c->at, cp);
}
static void cursor_init(JsonCursor *c, const char *s, const JsonToken *t, int index)
{
    c->s = s; c->at = (size_t)t[index].start + 1; c->end = (size_t)t[index].end;
}
void json_decode_init(JsonDecode *d, const char *s, const JsonToken *t, int index, char *out, size_t cap)
{
    memset(d, 0, sizeof(*d));
    d->out = out; d->cap = cap;
    if (index < 0 || t[index].type != JSON_STRING) d->failed = 1;
    else cursor_init(&d->in, s, t, index);
}
int json_decode_step(JsonDecode *d, size_t budget)
{
    size_t from = d->in.at;
    d->work = 0;
    if (d->failed) return -1;
    for (;;) {
        unsigned long cp;
        char bytes[4];
        int k, r;
        if (d->in.at != from && d->in.at - from + JSON_ATOM_MAX > budget) break;
        r = cursor_next(&d->in, &cp);
        if (r < 0 || (r && d->used >= d->cap)) { d->failed = 1; d->work = d->in.at - from; return -1; }
        if (r) { d->out[d->used] = 0; d->work = d->in.at - from; return 1; }
        k = utf8_emit(cp, bytes, sizeof(bytes));
        if (k < 0 || d->used >= d->cap || (size_t)k >= d->cap - d->used) { d->failed = 1; d->work = d->in.at - from; return -1; }
        memcpy(d->out + d->used, bytes, (size_t)k);
        d->used += (size_t)k;
    }
    d->work = d->in.at - from;
    return 0;
}

/* Does the key token spell `key`? cost accumulates the input bytes read. */
static int key_matches(const char *s, const JsonToken *t, int index, const char *key, size_t *cost)
{
    JsonCursor c;
    unsigned long cp;
    int r = 0;
    cursor_init(&c, s, t, index);
    for (; *key; key++) {
        r = cursor_next(&c, &cp);
        if (r || cp != (unsigned char)*key) break;
    }
    if (!*key) r = cursor_next(&c, &cp) == 1 ? 1 : 0;
    else r = 0;
    *cost += c.at - ((size_t)t[index].start + 1);
    return r;
}
void json_pick_init(JsonPick *p, const char *s, const JsonToken *t, int object,
                    const char *const *keys, int count)
{
    int i;
    memset(p, 0, sizeof(*p));
    p->s = s; p->t = t; p->keys = keys; p->count = count; p->object = object;
    for (i = 0; i < JSON_PICK_MAX; i++) p->found[i] = -1;
    p->child = object >= 0 && t[object].type == JSON_OBJECT ? object + 1 : -1;
}
int json_pick_step(JsonPick *p, size_t budget)
{
    size_t work = 0;
    while (p->child >= 0 && p->child < p->t[p->object].next) {
        size_t cost = 1;
        int k;
        if (work && work + JSON_PICK_NEED > budget) { p->work = work; return 0; }
        for (k = 0; k < p->count && k < JSON_PICK_MAX; k++)
            if (p->found[k] < 0 && key_matches(p->s, p->t, p->child, p->keys[k], &cost))
                p->found[k] = p->child + 1;
        work += cost;
        p->child = p->t[p->child + 1].next;
    }
    p->work = work;
    return 1;
}

/* Keys are decoded when compared, so escaped spellings collide. Each key is
 * hashed over its decoded code points and probed into a table sized to its
 * object; only a probe landing on an occupied slot compares text. */
#define KEY_BUFFER 8192
#define KEY_MAX 4096
enum { U_SCAN, U_COUNT, U_CLEAR, U_HASH, U_PROBE, U_COMPARE, U_DONE, U_FAIL };
void json_unique_init(JsonUnique *u, const char *s, const JsonToken *t, int count, uint16_t *slots)
{
    memset(u, 0, sizeof(*u));
    u->s = s; u->t = t; u->count = count; u->slots = slots;
    u->state = count < 0 || count > 65534 ? U_FAIL : U_SCAN;
}
static int code_point_bytes(unsigned long cp)
{
    return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}
static void unique_key(JsonUnique *u)
{
    cursor_init(&u->a, u->s, u->t, u->key);
    u->hash = 2166136261u; u->length = 0; u->probe = 0;
    u->state = U_HASH;
}
static void unique_advance(JsonUnique *u, size_t *work)
{
    const JsonToken *t = u->t;
    unsigned long cp, cpb;
    int r, rb;
    size_t from;
    switch (u->state) {
    case U_SCAN:
        *work += 1;
        if (u->object >= u->count) u->state = U_DONE;
        else if (t[u->object].type != JSON_OBJECT) u->object++;
        else { u->key = u->object + 1; u->keys = 0; u->state = U_COUNT; }
        break;
    case U_COUNT:
        *work += 1;
        if (u->key < t[u->object].next) {
            if (++u->keys > KEY_MAX) { u->state = U_FAIL; break; }
            u->key = t[u->key + 1].next;
        } else if (u->keys < 2) { u->object++; u->state = U_SCAN; }
        else {
            for (u->size = 4; u->size < 2 * u->keys; u->size *= 2) {}
            u->cleared = 0; u->state = U_CLEAR;
        }
        break;
    case U_CLEAR:
        *work += 1;
        u->slots[u->cleared++] = 0;
        if (u->cleared == u->size) { u->key = u->object + 1; unique_key(u); }
        break;
    case U_HASH:
        from = u->a.at;
        r = cursor_next(&u->a, &cp);
        *work += u->a.at - from;
        if (r < 0) { u->state = U_FAIL; break; }
        if (r) { u->state = U_PROBE; break; }
        u->length += (size_t)code_point_bytes(cp);
        if (u->length >= KEY_BUFFER) { u->state = U_FAIL; break; }
        u->hash = (u->hash ^ (uint32_t)cp) * 16777619u;
        break;
    case U_PROBE: {
        int at = (int)((u->hash + (uint32_t)u->probe) & (uint32_t)(u->size - 1));
        *work += 1;
        if (u->probe >= u->size) { u->state = U_FAIL; break; }
        if (!u->slots[at]) {
            u->slots[at] = (uint16_t)(u->key + 1);
            u->key = t[u->key + 1].next;
            if (u->key >= t[u->object].next) { u->object++; u->state = U_SCAN; }
            else unique_key(u);
        } else {
            cursor_init(&u->a, u->s, t, u->key);
            cursor_init(&u->b, u->s, t, u->slots[at] - 1);
            u->state = U_COMPARE;
        }
        break;
    }
    default: {
        size_t from_a = u->a.at, from_b = u->b.at;
        r = cursor_next(&u->a, &cp);
        rb = cursor_next(&u->b, &cpb);
        *work += (u->a.at - from_a) + (u->b.at - from_b);
        /* An undecodable key is refused as well as a repeat. */
        if (r < 0 || rb < 0 || (r && rb)) u->state = U_FAIL;
        else if (r || rb || cp != cpb) { u->probe++; u->state = U_PROBE; }
    }
    }
}
int json_unique_step(JsonUnique *u, size_t budget)
{
    size_t work = 0;
    while (u->state != U_DONE && u->state != U_FAIL) {
        if (work && work + JSON_ATOM_MAX > budget) break;
        unique_advance(u, &work);
    }
    u->work = work;
    return u->state == U_DONE ? 1 : u->state == U_FAIL ? -1 : 0;
}
int json_keys_unique(const char *s, const JsonToken *t, int count)
{
    static uint16_t slots[JSON_UNIQUE_SLOTS];
    JsonUnique u;
    json_unique_init(&u, s, t, count, slots);
    return json_unique_step(&u, JSON_UNLIMITED) == 1 ? 0 : -1;
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
int json_integer(const char *s, const JsonToken *t, int index, long *out)
{
    long v = 0;
    int i;
    if (index < 0 || t[index].type != JSON_PRIMITIVE || t[index].end <= t[index].start) return -1;
    for (i = t[index].start; i < t[index].end; i++) {
        int d = (unsigned char)s[i] - '0';
        if (d < 0 || d > 9 || v > (2147483647L - d) / 10) return -1;
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}
/* Rounds a non-negative decimal-or-exponent literal to millionths. The
 * mantissa is capped separately so no intermediate can overflow. */
int json_decimal_micros(const char *s, const JsonToken *t, int index, long long *out)
{
    static const long long pow10[19] = {
        1LL, 10LL, 100LL, 1000LL, 10000LL, 100000LL, 1000000LL, 10000000LL,
        100000000LL, 1000000000LL, 10000000000LL, 100000000000LL,
        1000000000000LL, 10000000000000LL, 100000000000000LL,
        1000000000000000LL, 10000000000000000LL, 100000000000000000LL,
        1000000000000000000LL
    };
    long long m = 0, divisor;
    long e = 0, scale;
    int i, end, frac = 0;
    if (index < 0 || t[index].type != JSON_PRIMITIVE || t[index].end <= t[index].start) return -1;
    i = t[index].start; end = t[index].end;
    if (s[i] == '-') return -1;                      /* usage costs are never negative */
    for (; i < end && s[i] >= '0' && s[i] <= '9'; i++) {
        int d = s[i] - '0';
        if (m > (1000000000000000000LL - d) / 10) return -1;
        m = m * 10 + d;
    }
    if (i < end && s[i] == '.') {
        i++;
        for (; i < end && s[i] >= '0' && s[i] <= '9'; i++) {
            int d = s[i] - '0';
            if (m > (1000000000000000000LL - d) / 10) return -1;
            m = m * 10 + d;
            frac++;
        }
    }
    if (i < end && (s[i] == 'e' || s[i] == 'E')) {
        int negative, digits = 0;
        i++;
        negative = i < end && s[i] == '-';
        if (i < end && (s[i] == '+' || s[i] == '-')) i++;
        for (; i < end && s[i] >= '0' && s[i] <= '9'; i++) {
            if (e < 1000) e = e * 10 + (s[i] - '0');
            digits++;
        }
        if (!digits) return -1;
        if (negative) e = -e;
    }
    if (i != end) return -1;
    scale = e - (long)frac + 6;
    if (scale >= 0) {
        if (scale > 18 || m > 999999999999999LL / pow10[scale]) return -1;
        m *= pow10[scale];
    } else if (-scale > 18) {
        m = 0;
    } else {
        divisor = pow10[-scale];
        m = (m + divisor / 2) / divisor;
    }
    *out = m;
    return 0;
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
