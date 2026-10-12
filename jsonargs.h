/* Bounded JSON argument and output helpers; no Toolbox dependencies. */
#ifndef SHERCLAWK_JSONARGS_H
#define SHERCLAWK_JSONARGS_H
#include "json.h"
/* A view of a buffer and its caller-owned cursor. The cursor may live in a
 * cooperative work state. Operations return 0/-1 and advance only on success. */
typedef struct { char *data; size_t capacity; size_t *cursor; } JsonBuf;
#define JSON_BUF(data, capacity, cursor) (&(JsonBuf){(data), (capacity), (cursor)})
int json_buf_append(JsonBuf *, const char *);
int json_buf_append_n(JsonBuf *, const char *, size_t);
int json_buf_quote(JsonBuf *, const char *);
/* Parsed, bounded tokens from json_parse. Allowed keys use |name| delimiters.
 * Reject unknown and duplicate decoded keys at the selected object. */
int json_args_check_keys(const char *, const JsonToken *, int object, const char *allowed);
int json_args_string(const char *, const JsonToken *, const char *name, char *, size_t);
/* Optional decimal integer, absent => default. Signs are allowed only when
 * low < 0; fractions/exponents, overflow and out-of-range values fail. */
int json_args_int(const char *, const JsonToken *, const char *name,
                  long def, long low, long high, long *out);
int json_args_error(char *, size_t, const char *code, const char *message, int native);
#endif
