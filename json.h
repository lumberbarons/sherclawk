/* Small strict JSON reader/writer. Tokens reference the original UTF-8 bytes. */
#ifndef SHERCLAWK_JSON_H
#define SHERCLAWK_JSON_H
#include <stddef.h>
enum { JSON_OBJECT = 1, JSON_ARRAY, JSON_STRING, JSON_PRIMITIVE };
typedef struct { int type, start, end, next; } JsonToken;
int json_parse(const char *s, size_t len, JsonToken *tokens, int cap);
int json_member(const char *s, const JsonToken *t, int object, const char *key);
int json_string(const char *s, const JsonToken *t, int index, char *out, size_t cap);
/* Strict numeric extraction from a parsed primitive token. json_integer takes
 * digits only, yielding [0, 2147483647]. json_decimal_micros takes a
 * non-negative decimal or exponent form and rounds to millionths, accepting
 * at most 999999999999999 micros. Both return 0 or -1. */
int json_integer(const char *s, const JsonToken *t, int index, long *out);
int json_decimal_micros(const char *s, const JsonToken *t, int index, long long *out);
int json_quote(const char *s, char *out, size_t cap);
int utf8_next(const char *s, size_t len, size_t *at, unsigned long *cp);
int utf8_emit(unsigned long cp, char *out, size_t cap);
#endif
