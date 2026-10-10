/* Bounded, resumable MCP processing: differential checks against the
 * synchronous implementations it replaced, then maximum-size fixtures that
 * assert every step stays within the work budget. No keys, no network. */
#include "mcp.h"
#include "mcp_sync.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Reference implementations: the recursive parser, the qsort key check
 * and the strstr redactor exactly as they were before this change. ---- */
typedef struct { const char *s; size_t n, at; JsonToken *t; int used, cap; } Reader;
static int ref_hex4(const char *s, size_t n, size_t *at, unsigned long *v)
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
static int ref_string(const char *s, size_t n, size_t *at, char *out, size_t cap)
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
                if (ref_hex4(s, n, at, &c)) return -1;
                if (c >= 0xd800 && c <= 0xdbff) {
                    if (n - *at < 6 || s[*at] != '\\' || s[*at + 1] != 'u') return -1;
                    *at += 2;
                    if (ref_hex4(s, n, at, &lo) || lo < 0xdc00 || lo > 0xdfff) return -1;
                    c = 0x10000 + ((c - 0xd800) << 10) + lo - 0xdc00;
                } else if (c >= 0xdc00 && c <= 0xdfff) return -1;
                break;
            }
            default: return -1;
            }
        } else if ((unsigned char)s[*at] < 32 || utf8_next(s, n, at, &c)) return -1;
        if (!c) return -1;
        k = utf8_emit(c, bytes, sizeof(bytes));
        if (k < 0 || (out && (used >= cap || (size_t)k >= cap - used))) return -1;
        if (out) memcpy(out + used, bytes, (size_t)k);
        used += (size_t)k;
    }
    return -1;
}
static void ref_ws(Reader *r)
{
    while (r->at < r->n && (r->s[r->at] == ' ' || r->s[r->at] == '\r' ||
           r->s[r->at] == '\n' || r->s[r->at] == '\t')) r->at++;
}
static int ref_value(Reader *r, int depth)
{
    int id, kind;
    char c;
    size_t start;
    ref_ws(r);
    if (depth > 32 || r->at >= r->n || r->used == r->cap) return -1;
    id = r->used++; start = r->at; c = r->s[r->at];
    kind = c == '{' ? JSON_OBJECT : c == '[' ? JSON_ARRAY : c == '"' ? JSON_STRING : JSON_PRIMITIVE;
    r->t[id].type = kind; r->t[id].start = (int)start;
    if (kind == JSON_STRING) {
        if (ref_string(r->s, r->n, &r->at, NULL, 0) < 0) return -1;
    } else if (kind == JSON_OBJECT || kind == JSON_ARRAY) {
        char end = kind == JSON_OBJECT ? '}' : ']';
        r->at++; ref_ws(r);
        if (r->at < r->n && r->s[r->at] == end) r->at++;
        else for (;;) {
            if (kind == JSON_OBJECT) {
                ref_ws(r);
                if (r->at >= r->n || r->s[r->at] != '"') return -1;
                if (ref_value(r, depth + 1) < 0) return -1;
                ref_ws(r);
                if (r->at >= r->n || r->s[r->at++] != ':') return -1;
            }
            if (ref_value(r, depth + 1) < 0) return -1;
            ref_ws(r);
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
static int ref_parse(const char *s, size_t len, JsonToken *tokens, int cap)
{
    Reader r = {s, len, 0, tokens, 0, cap};
    if (ref_value(&r, 0) < 0) return -1;
    ref_ws(&r);
    return r.at == len ? r.used : -1;
}
static const char *key_text;
static const JsonToken *key_tokens;
static int key_error;
static char key_a[8192], key_b[8192];
static int ref_string_at(const char *s, const JsonToken *t, int index, char *out, size_t cap)
{
    size_t at;
    if (index < 0 || t[index].type != JSON_STRING) return -1;
    at = (size_t)t[index].start;
    return ref_string(s, (size_t)t[index].end, &at, out, cap);
}
static int key_compare(const void *x, const void *y)
{
    if (ref_string_at(key_text, key_tokens, *(const int *)x, key_a, sizeof(key_a)) < 0 ||
        ref_string_at(key_text, key_tokens, *(const int *)y, key_b, sizeof(key_b)) < 0) {
        key_error = 1; return 0;
    }
    return strcmp(key_a, key_b);
}
static int ref_keys_unique(const char *s, const JsonToken *t, int count)
{
    static int order[4096];
    int i, j, n;
    key_text = s; key_tokens = t; key_error = 0;
    for (i = 0; i < count; i++) if (t[i].type == JSON_OBJECT) {
        n = 0;
        for (j = i + 1; j < t[i].next; j = t[j + 1].next) {
            if (n == 4096) return -1;
            order[n++] = j;
        }
        qsort(order, (size_t)n, sizeof(order[0]), key_compare);
        if (key_error) return -1;
        for (j = 1; j < n; j++) if (!key_compare(&order[j - 1], &order[j])) return -1;
    }
    return 0;
}
static void ref_redact_value(char *s, const char *secret)
{
    char *p;
    size_t n = strlen(secret);
    if (!n) return;
    while ((p = strstr(s, secret)) != NULL) { memset(p, '*', n); s = p + n; }
}
static void ref_redact(const McpConfig *c, const char *session, char *s)
{
    size_t at = 0;
    if (session) ref_redact_value(s, session);
    while (at < c->secrets_len) {
        const char *v = c->secrets + at;
        const char *payload = strchr(v, ' ');
        size_t n = strlen(v);
        ref_redact_value(s, v);
        if (payload && payload[1]) ref_redact_value(s, payload + 1);
        at += n + 1;
    }
}

/* ---- Deterministic randomness. ---- */
/* Fixtures write into buffers sized by construction; snprintf is the non-deprecated spelling. */
static size_t emit(char *buffer, const char *format, ...) __attribute__((format(printf, 2, 3)));
static size_t emit(char *buffer, const char *format, ...)
{
    va_list args;
    int n;
    va_start(args, format);
    n = vsnprintf(buffer, (size_t)1 << 30, format, args);
    va_end(args);
    assert(n >= 0);
    return (size_t)n;
}
static unsigned long seed = 12345;
static unsigned rnd(unsigned n)
{
    seed = seed * 1103515245UL + 12345UL;
    return (unsigned)((seed >> 16) & 0x7fff) % n;
}

static JsonToken ref_tokens[9000], new_tokens[9000];
static const size_t budgets[] = {(size_t)-1, 1, 24, 37, 100, 8192};
#define BUDGETS ((int)(sizeof(budgets) / sizeof(budgets[0])))

static void parse_agrees(const char *s, size_t len, int cap)
{
    int want = ref_parse(s, len, ref_tokens, cap), b;
    for (b = 0; b < BUDGETS; b++) {
        JsonParser p;
        int r, i, steps = 0;
        json_parser_init(&p, s, len, new_tokens, cap);
        while (!(r = json_parser_step(&p, budgets[b]))) {
            assert(budgets[b] < JSON_ATOM_MAX || p.work <= budgets[b]);
            assert(++steps <= (int)len + 8);
        }
        assert(budgets[b] < JSON_ATOM_MAX || p.work <= budgets[b]);
        assert((r > 0 ? p.used : -1) == want);
        for (i = 0; i < want; i++) {
            assert(ref_tokens[i].type == new_tokens[i].type && ref_tokens[i].start == new_tokens[i].start &&
                   ref_tokens[i].end == new_tokens[i].end && ref_tokens[i].next == new_tokens[i].next);
        }
    }
}
static void parser_matches_reference(void)
{
    static const char *const seeds[] = {
        "{\"a\":[1,2.5e-3,{\"b\":null,\"c\":\"x\\u00e9\\ud83d\\ude00\"}],\"d\":true,\"e\":false}",
        "[]", "{}", "[[],{},[{}]]", "  \"caf\xc3\xa9 \\n \\\"q\\\" \"  ", "-0.5E+10", "[0,10,-1,1.0,1e5]",
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"hi\"}],\"isError\":false}}",
    };
    static const char alphabet[] = "{}[]\",:\\-+0123456789.eEtrufalsnb \n\t\xc3\xa9u";
    char text[256];
    int i, k, depth;
    for (i = 0; i < (int)(sizeof(seeds) / sizeof(seeds[0])); i++) {
        size_t n = strlen(seeds[i]);
        for (k = 0; k <= (int)n; k++) parse_agrees(seeds[i], (size_t)k, 64);
        parse_agrees(seeds[i], n, 3);
        parse_agrees(seeds[i], n, 7);
    }
    for (i = 0; i < 6000; i++) {
        const char *base = seeds[rnd(sizeof(seeds) / sizeof(seeds[0]))];
        size_t n = strlen(base);
        int edits = 1 + (int)rnd(4);
        memcpy(text, base, n);
        for (k = 0; k < edits; k++) text[rnd((unsigned)n)] = alphabet[rnd(sizeof(alphabet) - 1)];
        text[n] = 0;
        parse_agrees(text, n, 64);
    }
    /* The 32-level ceiling: containers nest to 33 deep, values inside the last do not. */
    for (depth = 30; depth <= 36; depth++) {
        char deep[200];
        size_t at = 0;
        for (k = 0; k < depth; k++) deep[at++] = '[';
        deep[at++] = '1';
        for (k = 0; k < depth; k++) deep[at++] = ']';
        deep[at] = 0;
        parse_agrees(deep, at, 64);
        at = 0;
        for (k = 0; k < depth; k++) deep[at++] = '[';
        for (k = 0; k < depth; k++) deep[at++] = ']';
        deep[at] = 0;
        parse_agrees(deep, at, 64);
    }
    /* An over-long input is refused, as is one more token than the cap. */
    parse_agrees("[1,2,3]", 7, 4);
    parse_agrees("[1,2,3]", 7, 3);
}
static void unique_agrees(const char *s, size_t len)
{
    static uint16_t slots[JSON_UNIQUE_SLOTS];
    int n = json_parse(s, len, new_tokens, 9000), b;
    int want;
    if (n < 1) return;
    want = ref_keys_unique(s, new_tokens, n);
    for (b = 0; b < BUDGETS; b++) {
        JsonUnique u;
        int r, steps = 0;
        json_unique_init(&u, s, new_tokens, n, slots);
        while (!(r = json_unique_step(&u, budgets[b]))) {
            assert(budgets[b] < JSON_ATOM_MAX || u.work <= budgets[b]);
            assert(++steps < 4000000);
        }
        assert(budgets[b] < JSON_ATOM_MAX || u.work <= budgets[b]);
        assert((r > 0 ? 0 : -1) == want);
    }
    assert(json_keys_unique(s, new_tokens, n) == want);
}
static void keys_match_reference(void)
{
    static const char *const spell[] = {"a", "\\u0061", "b", "\\u00e9", "\xc3\xa9", "\\ud83d\\ude00", "\xf0\x9f\x98\x80",
        "ab", "a\\u0062", "", "k\\/", "k/", "\\n", "\n_"};
    static char text[4096];
    int i, k, depth;
    for (i = 0; i < 3000; i++) {
        size_t at = 0;
        int keys = 1 + (int)rnd(7), nest = (int)rnd(3);
        at += emit(text + at, "{");
        for (k = 0; k < keys; k++) {
            const char *name = spell[rnd(sizeof(spell) / sizeof(spell[0]))];
            /* Raw newlines are not valid inside strings; keep them escaped. */
            if (name[0] == '\n') name = "x";
            at += emit(text + at, "%s\"%s\":", k ? "," : "", name);
            if (nest && !rnd(2)) {
                at += emit(text + at, "{\"%s\":1,\"%s\":2}", spell[rnd(4)], spell[rnd(4)]);
                nest--;
            } else at += emit(text + at, "%d", k);
        }
        at += emit(text + at, "}");
        unique_agrees(text, at);
    }
    /* Arrays of objects: the same key in siblings is not a repeat. */
    unique_agrees("[{\"a\":1},{\"a\":2},{\"a\":3,\"b\":4}]", 33);
    unique_agrees("{\"a\":{\"a\":{\"a\":1}}}", 21);
    /* Many keys, with colliding table slots: 4096 distinct is fine, one more is not. */
    for (depth = 4094; depth <= 4097; depth++) {
        static char big[200000];
        size_t at = emit(big, "{");
        for (k = 0; k < depth; k++) at += emit(big + at, "%s\"key%d\":0", k ? "," : "", k);
        at += emit(big + at, "}");
        unique_agrees(big, at);
        /* a repeat spelled differently, at the very end */
        emit(big + at - 1, ",\"k\\u0065y%d\":1}", depth - 1);
        unique_agrees(big, strlen(big));
    }
    /* Undecodable-length keys: a lone long key is not examined, a pair is refused. */
    {
        static char longkey[20000];
        size_t at = emit(longkey, "{\"");
        memset(longkey + at, 'k', 8192); at += 8192;
        at += emit(longkey + at, "\":1}");
        unique_agrees(longkey, at);
        at -= 1;
        at += emit(longkey + at, ",\"b\":2}");
        unique_agrees(longkey, at);
        memset(longkey + 2, 'k', 8190);
        unique_agrees(longkey, at);
    }
}
static void redaction_matches_reference(void)
{
    static McpConfig config;
    static const char configuration[] = "{\"mcpServers\":{\"srv\":{\"url\":\"https://x.test/mcp?token=querysecret1\","
        "\"headers\":{\"Authorization\":\"Bearer abcdefghij\",\"X-Api-Key\":\"key-12345678\"}}}}";
    static const char *const pieces[] = {"Bearer abcdefghij", "abcdefghij", "Bearer abcdefghi", "key-12345678", "key-1234567",
        "querysecret1", "sess-xyz", "ab", " ", "Bearer ", "*", "e", "Bearer Bearer abcdefghij", "key-12345678key-12345678"};
    char error[128], text[400], want[400], got[400];
    int i, k, b;
    assert(!mcp_config_parse(configuration, strlen(configuration), &config, error, sizeof(error)));
    for (i = 0; i < 4000; i++) {
        const char *session = rnd(3) ? "sess-xyz" : NULL;
        text[0] = 0;
        for (k = 0; k < 1 + (int)rnd(6); k++) strcat(text, pieces[rnd(sizeof(pieces) / sizeof(pieces[0]))]);
        strcpy(want, text);
        ref_redact(&config, session, want);
        for (b = 0; b < BUDGETS; b++) {
            McpRedact r;
            int steps = 0;
            strcpy(got, text);
            mcp_redact_init(&r, &config, session, got);
            while (!mcp_redact_step(&r, budgets[b])) {
                assert(r.work <= budgets[b]);
                assert(++steps < 4000000);
            }
            assert(!strcmp(got, want));
        }
    }
}

/* ---- Bounded work on maximum-size messages. ---- */
static McpConfig tavily, generic;
static McpRegistry registry;
static char reply[2048];
static char message[MCP_MESSAGE_CAP + 8];

static void expect_bounded(const McpWork *w, size_t length)
{
    /* Every step stayed within the budget, and the whole message was read
     * across enough of them that no step could have read it at once. */
    assert(w->peak <= MCP_WORK_BUDGET);
    assert(w->units >= length);
    assert(w->steps >= (length + MCP_WORK_BUDGET - 1) / MCP_WORK_BUDGET);
}
static int classify_call(const char *text, size_t length)
{
    mcp_work_init(&sync_work, MCP_STAGE_CLASSIFY | MCP_STAGE_CALL, &tavily, &registry, text, length, 5, reply, sizeof(reply), NULL, 0);
    sync_run(MCP_WORK_BUDGET);
    return sync_work.rpc == 1 && sync_work.status == 0 ? 0 : -1;
}
static size_t call_response(char *out, size_t total, int is_error)
{
    /* A result padded with one long string to exactly `total` bytes. */
    static const char head[] = "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"";
    char tail[64];
    size_t pad, tail_len;
    tail_len = emit(tail, "\"}],\"isError\":%s}}", is_error ? "true" : "false");
    memcpy(out, head, sizeof(head) - 1);
    pad = total - (sizeof(head) - 1) - tail_len;
    memset(out + sizeof(head) - 1, 'x', pad);
    memcpy(out + sizeof(head) - 1 + pad, tail, tail_len + 1);
    return total;
}
static void maximum_messages(void)
{
    size_t n = call_response(message, MCP_MESSAGE_CAP, 0);
    int i;
    assert(strlen(message) == MCP_MESSAGE_CAP);
    assert(!classify_call(message, n));
    assert(!sync_work.call_error);
    expect_bounded(&sync_work, n);
    n = call_response(message, MCP_MESSAGE_CAP, 1);
    assert(!classify_call(message, n) && sync_work.call_error);
    expect_bounded(&sync_work, n);
    /* One byte more than the cap is refused before any parsing. */
    n = call_response(message, MCP_MESSAGE_CAP + 1, 0);
    assert(classify_call(message, n) < 0 && sync_work.rpc < 0 && sync_work.units < MCP_WORK_BUDGET);
    /* Text of the same size made of escapes and multi-byte characters. */
    {
        size_t at = emit(message, "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"content\":[{\"text\":\"");
        while (at < MCP_MESSAGE_CAP - 80) at += emit(message + at, "\\u00e9\\ud83d\\ude00\xc3\xa9\\n");
        at += emit(message + at, "\"}]}}");
        assert(!classify_call(message, at));
        expect_bounded(&sync_work, at);
    }
    /* Exactly the token cap parses; one more token is refused. */
    for (i = 8183; i <= 8184; i++) {
        size_t at = emit(message, "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"v\":[");
        int k;
        for (k = 0; k < i; k++) at += emit(message + at, "%s0", k ? "," : "");
        at += emit(message + at, "]}}");
        assert((classify_call(message, at) == 0) == (i == 8183));
        assert(sync_work.peak <= MCP_WORK_BUDGET);
    }
    /* A server request with a long id is answered, or refused, without reading past the reply buffer. */
    {
        size_t at = emit(message, "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":\"");
        memset(message + at, 'i', 3000); at += 3000;
        at += emit(message + at, "\"}");
        assert(rpc(message, 1, reply, sizeof(reply)) < 0);
        assert(rpc("{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":-5}", 1, reply, sizeof(reply)) == 2);
        assert(!strcmp(reply, "{\"jsonrpc\":\"2.0\",\"id\":-5,\"result\":{}}"));
    }
}
static void duplicate_keys(void)
{
    int k;
    size_t at;
    /* The largest object a message can hold: all keys distinct. */
    at = emit(message, "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{");
    for (k = 0; k < 4000; k++) at += emit(message + at, "%s\"key%04d\":%d", k ? "," : "", k, k);
    at += emit(message + at, "}}");
    assert(!classify_call(message, at));
    assert(sync_work.peak <= MCP_WORK_BUDGET && sync_work.steps > 4);
    /* The same object with a repeat spelled differently, last of all. */
    strcpy(message + at - 2, ",\"k\\u0065y0007\":0}}");
    assert(classify_call(message, strlen(message)) < 0 && sync_work.rpc < 0);
    assert(sync_work.peak <= MCP_WORK_BUDGET);
    /* A repeat in a nested object, and keys that differ only at their end. */
    assert(rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"a\":{\"x\":1,\"x\":2}}}", 1, reply, sizeof(reply)) < 0);
    at = emit(message, "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{");
    for (k = 0; k < 40; k++) {
        int j;
        at += emit(message + at, "%s\"", k ? "," : "");
        for (j = 0; j < 1400; j++) message[at++] = 'p';
        at += emit(message + at, "%02d\":0", k);
    }
    at += emit(message + at, "}}");
    assert(!classify_call(message, at));
    assert(sync_work.peak <= MCP_WORK_BUDGET && sync_work.steps >= (at + MCP_WORK_BUDGET - 1) / MCP_WORK_BUDGET);
    strcpy(message + at - 2, ",\"");
    memset(message + at, 'p', 1400); at += 1400;
    at += emit(message + at, "07\":0}}");
    assert(classify_call(message, at) < 0 && sync_work.peak <= MCP_WORK_BUDGET);
}

/* Discovery: tools whose descriptions need decoding, redacting and quoting,
 * padded with ignored members so the whole page is near the message cap. */
static const char secret_text[] = "Bearer sekret-token-0123";
static size_t tool_entry(char *out, int index, size_t pad, int readonly)
{
    size_t at = emit(out, "{\"name\":\"tool%d\",\"title\":\"", index);
    memset(out + at, 't', pad); at += pad;
    at += emit(out + at, "\",\"description\":\"Query \\\"quoted\\\" and back\\\\slash \\u0041 %s tail \\n end\"", secret_text);
    if (readonly) at += emit(out + at, ",\"annotations\":{\"readOnlyHint\":true}");
    at += emit(out + at, ",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"q\":{\"type\":\"string\"}}}}");
    return at;
}
static void discovery_pages(void)
{
    static char other[MCP_SCHEMA_CAP + 1];
    char expected[600];
    size_t at;
    int k, b;
    const size_t discover_budgets[] = {MCP_WORK_BUDGET, 4000, 500, 24, 1};
    at = emit(message, "{\"result\":{\"tools\":[");
    for (k = 0; k < 4; k++) {
        if (k) message[at++] = ',';
        at += tool_entry(message + at, k, 14000, 1);
    }
    at += emit(message + at, "],\"nextCursor\":\"page-two\"}}");
    assert(at > 56000 && at < MCP_MESSAGE_CAP);
    emit(expected, "{\"type\":\"function\",\"function\":{\"name\":\"mcp_srv_tool0\",\"description\":"
        "\"Query \\\"quoted\\\" and back\\\\slash A ************************ tail \\u000a end\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"q\":{\"type\":\"string\"}}}}}");
    for (b = 0; b < 5; b++) {
        memset(&registry, 0, sizeof(registry));
        assert(discover_with(&generic, &registry, message, discover_budgets[b]) == 0);
        assert(registry.count == 4 && !strcmp(registry.cursor, "page-two") && registry.entries == 4);
        assert(!strncmp(registry.schemas, expected, strlen(expected)));
        assert(!strstr(registry.schemas, "sekret"));
        assert(!strcmp(registry.tools[3].name, "mcp_srv_tool3") && !strcmp(registry.tools[3].original, "tool3"));
        if (discover_budgets[b] == MCP_WORK_BUDGET) expect_bounded(&sync_work, at);
        if (b) assert(!strcmp(registry.schemas, other));
        else strcpy(other, registry.schemas);
    }
    /* The schema budget is checked from sizes alone: nothing is half-registered. */
    at = emit(message, "{\"result\":{\"tools\":[");
    for (k = 0; k < 8; k++) {
        if (k) message[at++] = ',';
        at += tool_entry(message + at, k, 100, 1);
    }
    at += emit(message + at, "]}}");
    memset(&registry, 0, sizeof(registry));
    assert(discover_page(&generic, &registry, message) == 1 && registry.count == 8);
    assert(registry.schema_len == strlen(registry.schemas) && registry.schema_len < MCP_SCHEMA_CAP);
    /* Descriptions: 8191 decoded characters fit the decoder, 8192 do not (and only skip the tool). */
    for (k = 8191; k <= 8192; k++) {
        at = emit(message, "{\"result\":{\"tools\":[{\"name\":\"d\",\"annotations\":{\"readOnlyHint\":true},\"inputSchema\":{},\"description\":\"");
        memset(message + at, 'd', (size_t)k); at += (size_t)k;
        at += emit(message + at, "\"}]}}");
        memset(&registry, 0, sizeof(registry));
        if (k == 8191) {
            /* 8 KiB of description is within the 16 KiB schema allowance. */
            assert(discover_page(&generic, &registry, message) == 1 && registry.count == 1);
        } else {
            assert(discover_page(&generic, &registry, message) == 1 && registry.count == 0);
            assert(strstr(registry.notice, "unsupported description"));
        }
    }
    /* A schema that cannot fit discards the page, whatever the budget. */
    at = emit(message, "{\"result\":{\"tools\":[{\"name\":\"s\",\"annotations\":{\"readOnlyHint\":true},\"inputSchema\":{\"p\":\"");
    memset(message + at, 's', MCP_SCHEMA_CAP); at += MCP_SCHEMA_CAP;
    at += emit(message + at, "\"}}]}}");
    for (b = 0; b < 5; b++) {
        memset(&registry, 0, sizeof(registry));
        assert(discover_with(&generic, &registry, message, discover_budgets[b]) < 0);
        assert(!registry.count && !registry.schema_len && strstr(registry.notice, "schema limit"));
    }
    /* A full discovery of 64 entries across one page. */
    at = emit(message, "{\"result\":{\"tools\":[");
    for (k = 0; k < MCP_ENTRY_MAX; k++) at += emit(message + at, "%s{\"name\":\"n%d\",\"inputSchema\":{}}", k ? "," : "", k);
    at += emit(message + at, "]}}");
    memset(&registry, 0, sizeof(registry));
    assert(discover_page(&generic, &registry, message) == 1 && registry.count == 0 && registry.entries == MCP_ENTRY_MAX);
    assert(sync_work.peak <= MCP_WORK_BUDGET);
}
static void initialize_bounds(void)
{
    char version[16];
    size_t at = emit(message, "{\"result\":{\"protocolVersion\":\"2025-06-18\",\"instructions\":\"");
    memset(message + at, 'i', 60000); at += 60000;
    at += emit(message + at, "\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"fixture\",\"version\":\"1\"}}}");
    assert(!initialize_result(message, version, sizeof(version)) && !strcmp(version, "2025-06-18"));
    assert(sync_work.peak <= MCP_WORK_BUDGET && sync_work.steps >= 7);
    /* A version longer than the destination is refused rather than truncated. */
    assert(initialize_result("{\"result\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"f\",\"version\":\"1\"}}}", version, 5) < 0);
}
int main(void)
{
    static const char tavily_config[] = "{\"mcpServers\":{\"tavily\":{\"url\":\"https://mcp.tavily.com/mcp/\"}}}";
    static const char generic_config[] = "{\"mcpServers\":{\"srv\":{\"url\":\"https://example.test/mcp\","
        "\"headers\":{\"Authorization\":\"Bearer sekret-token-0123\"}}}}";
    char error[128];
    assert(!mcp_config_parse(tavily_config, strlen(tavily_config), &tavily, error, sizeof(error)));
    assert(!mcp_config_parse(generic_config, strlen(generic_config), &generic, error, sizeof(error)));
    parser_matches_reference();
    keys_match_reference();
    redaction_matches_reference();
    maximum_messages();
    duplicate_keys();
    discovery_pages();
    initialize_bounds();
    puts("PASS resumable MCP parsing, key checks, redaction and discovery match the synchronous reference within the work budget");
    return 0;
}
