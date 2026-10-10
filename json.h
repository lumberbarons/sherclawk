/* Small strict JSON reader/writer. Tokens reference the original UTF-8 bytes. */
#ifndef SHERCLAWK_JSON_H
#define SHERCLAWK_JSON_H
#include <stddef.h>
#include <stdint.h>
enum { JSON_OBJECT = 1, JSON_ARRAY, JSON_STRING, JSON_PRIMITIVE };
typedef struct { int type, start, end, next; } JsonToken;
int json_parse(const char *s, size_t len, JsonToken *tokens, int cap);
int json_member(const char *s, const JsonToken *t, int object, const char *key);
int json_string(const char *s, const JsonToken *t, int index, char *out, size_t cap);
/* Synchronous wrapper over the resumable checker below, for small documents
 * (configuration). 0 when no object among the first count tokens repeats a
 * key (compared after decoding escapes); -1 for a duplicate, undecodable or
 * over-long (8 KiB) key, or more than 4096 keys in one object. Uses static
 * scratch; not reentrant. */
int json_keys_unique(const char *s, const JsonToken *t, int count);

/* Resumable processing. Work is counted in units: an input byte consumed, or
 * a token, key or table slot touched. A step ends before an atom that would
 * overrun its budget, but always completes one atom, so for a budget of at
 * least JSON_ATOM_MAX no step exceeds it; `work` is the units of the latest
 * step. Steppers return 1 when finished, 0 when more work remains, and -1 on
 * failure; after -1 or 1 they must not be stepped again. */
#define JSON_ATOM_MAX 24
#define JSON_DEPTH 32
#define JSON_PICK_MAX 6
#define JSON_PICK_NEED 1536 /* one child: every wanted key compared, 12 bytes per character */
#define JSON_UNIQUE_SLOTS 8192

/* json_parse as an explicit state machine: a 32-level container stack, no
 * recursion. `used` is the token count once finished. */
typedef struct {
    const char *s;
    size_t n, at, work;
    JsonToken *t;
    int used, cap, state, sub, current, depth;
    int open[JSON_DEPTH + 1];
} JsonParser;
void json_parser_init(JsonParser *p, const char *s, size_t len, JsonToken *tokens, int cap);
int json_parser_step(JsonParser *p, size_t budget);

/* Duplicate-key detection with one hash table per object, so the work is
 * linear in key bytes rather than O(n log n) decodes. The caller owns the
 * JSON_UNIQUE_SLOTS table; only one checker may use a table at a time. */
typedef struct { const char *s; size_t at, end; } JsonCursor;
typedef struct {
    const char *s;
    const JsonToken *t;
    uint16_t *slots;
    int count, state, object, key, keys, size, cleared, probe;
    uint32_t hash, seed;
    size_t length, work;
    JsonCursor a, b;
} JsonUnique;
/* `seed` starts every key hash, so a server that cannot predict it cannot
 * choose keys that share a probe chain. */
void json_unique_init(JsonUnique *u, const char *s, const JsonToken *t, int count, uint16_t *slots, uint32_t seed);
int json_unique_step(JsonUnique *u, size_t budget);

/* Resumable json_member for up to JSON_PICK_MAX keys at once: one pass over
 * the object's children records, per key, the first matching value token (or
 * -1) in found[]. */
typedef struct {
    const char *s;
    const JsonToken *t;
    const char *const *keys;
    int count, object, child, found[JSON_PICK_MAX];
    size_t work;
} JsonPick;
void json_pick_init(JsonPick *p, const char *s, const JsonToken *t, int object,
                    const char *const *keys, int count);
int json_pick_step(JsonPick *p, size_t budget);

/* Resumable json_string: decodes into out (NUL-terminated, cap bytes); -1 when
 * not a string, invalid or longer than cap - 1. `used` is the length. */
typedef struct {
    JsonCursor in;
    char *out;
    size_t cap, used, work;
    int failed;
} JsonDecode;
void json_decode_init(JsonDecode *d, const char *s, const JsonToken *t, int index, char *out, size_t cap);
int json_decode_step(JsonDecode *d, size_t budget);

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
