#include "agent/jsonargs.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int json_buf_append_n(JsonBuf *b, const char *s, size_t n)
{
    size_t at = *b->cursor;
    if (at >= b->capacity || n >= b->capacity - at) return -1;
    memcpy(b->data + at, s, n); b->data[at + n] = 0;
    *b->cursor += n; return 0;
}
int json_buf_append(JsonBuf *b, const char *s)
{ return json_buf_append_n(b, s, strlen(s)); }
int json_buf_quote(JsonBuf *b, const char *s)
{
    int n;
    if (*b->cursor >= b->capacity) return -1;
    n = json_quote(s, b->data + *b->cursor, b->capacity - *b->cursor);
    if (n < 0) return -1;
    *b->cursor += (size_t)n; return 0;
}
int json_args_check_keys(const char *s, const JsonToken *t, int object, const char *allowed)
{
    char key[64], previous[64], padded[68];
    int i, j;
    if (object < 0 || t[object].type != JSON_OBJECT) return -1;
    for (i = object + 1; i < t[object].next; i = t[i + 1].next) {
        if (json_string(s, t, i, key, sizeof(key)) < 0) return -1;
        /* A decoded delimiter could otherwise match two allowed keys. */
        if (!key[0] || strchr(key, '|')) return -1;
        snprintf(padded, sizeof(padded), "|%s|", key);
        if (!strstr(allowed, padded)) return -1;
        for (j = object + 1; j < i; j = t[j + 1].next)
            if (json_string(s, t, j, previous, sizeof(previous)) < 0 || !strcmp(previous, key)) return -1;
    }
    return 0;
}
int json_args_string(const char *s, const JsonToken *t, const char *name, char *out, size_t cap)
{ return json_string(s, t, json_member(s, t, 0, name), out, cap); }
int json_args_int(const char *s, const JsonToken *t, const char *name,
                  long def, long low, long high, long *out)
{
    char value[32];
    long n;
    int i = json_member(s, t, 0, name), k = 0, size;
    if (i < 0) { *out = def; return 0; }
    size = t[i].end - t[i].start;
    if (t[i].type != JSON_PRIMITIVE || size < 1 || size >= (int)sizeof(value)) return -1;
    if (low < 0 && s[t[i].start] == '-') k = 1;
    if (k == size) return -1;
    for (; k < size; k++) if (s[t[i].start + k] < '0' || s[t[i].start + k] > '9') return -1;
    memcpy(value, s + t[i].start, (size_t)size); value[size] = 0;
    errno = 0; n = strtol(value, NULL, 10);
    if (errno == ERANGE || n < low || n > high) return -1;
    *out = n; return 0;
}
int json_args_error(char *out, size_t cap, const char *code, const char *message, int native)
{
    char q[512];
    if (json_quote(message, q, sizeof(q)) < 0) strcpy(q, "\"Tool failed\"");
    snprintf(out, cap, "{\"status\":\"error\",\"code\":\"%s\",\"message\":%s,\"os_error\":%d}", code, q, native);
    return -1;
}
