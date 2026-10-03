/* Small strict JSON reader/writer. Tokens reference the original UTF-8 bytes. */
#ifndef SHERCLAWK_JSON_H
#define SHERCLAWK_JSON_H
#include <stddef.h>
enum { JSON_OBJECT = 1, JSON_ARRAY, JSON_STRING, JSON_PRIMITIVE };
typedef struct { int type, start, end, next; } JsonToken;
int json_parse(const char *s, size_t len, JsonToken *tokens, int cap);
int json_member(const char *s, const JsonToken *t, int object, const char *key);
int json_string(const char *s, const JsonToken *t, int index, char *out, size_t cap);
int json_quote(const char *s, char *out, size_t cap);
int utf8_next(const char *s, size_t len, size_t *at, unsigned long *cp);
int utf8_emit(unsigned long cp, char *out, size_t cap);
#endif
