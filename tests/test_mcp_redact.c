/* Credential redaction across decoded and escaped reflections: the plain-text
 * helper against an independent reference, and the schema and call-result
 * stages end to end. Each fixture states what the model may see afterwards. */
#include "mcp.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static McpWork sync_work;
static void sync_run(size_t budget)
{
    while (!mcp_work_step(&sync_work, budget)) assert(sync_work.work <= budget || budget < JSON_PICK_NEED);
    assert(sync_work.work <= budget || budget < JSON_PICK_NEED);
}

/* The header value is  Bearer ab"c\d/e  and the key  k9Azz  (spelled with a
 * \u0041 in the file); the query carries  querysecret1. */
static const char configuration[] = "{\"mcpServers\":{\"srv\":{\"url\":\"https://x.test/mcp?token=querysecret1\","
    "\"headers\":{\"Authorization\":\"Bearer ab\\\"c\\\\d/e\",\"X-Key\":\"k9\\u0041zz\"}}}}";
static const char *const secrets[] = {"Bearer ab\"c\\d/e", "ab\"c\\d/e", "k9Azz", "querysecret1"};
#define SECRET_COUNT 4
#define SESSION "sess-Q7x/9"
static McpConfig config;
static McpRegistry registry;
static char message[MCP_MESSAGE_CAP + 8], copy[MCP_MESSAGE_CAP + 8], reply[256];

/* ---- Deterministic randomness. ---- */
static uint32_t state = 12345;
static uint32_t rnd(uint32_t n)
{
    state = state * 1664525u + 1013904223u;
    return (state >> 8) % n;
}
static size_t emit(char *out, const char *format, ...) __attribute__((format(printf, 2, 3)));
static size_t emit(char *out, const char *format, ...)
{
    va_list args;
    int n;
    va_start(args, format);
    n = vsnprintf(out, (size_t)1 << 30, format, args);
    va_end(args);
    assert(n >= 0);
    return (size_t)n;
}
/* One character as a JSON string would spell it, picked at random. */
static size_t spell(char *out, char c)
{
    int pick = (int)rnd(4);
    if ((c == '"' || c == '\\' || c == '/') && pick == 0) return emit(out, "\\%c", c);
    if (c != '"' && c != '\\' && pick == 0) { out[0] = c; return 1; }
    if (pick == 1 || pick == 2) return emit(out, pick == 1 ? "\\u00%02x" : "\\u00%02X", (unsigned)(unsigned char)c);
    if (c == '"' || c == '\\') return emit(out, "\\%c", c);
    out[0] = c; return 1;
}
static size_t encode(char *out, const char *plain)
{
    size_t at = 0;
    for (; *plain; plain++) at += spell(out + at, *plain);
    out[at] = 0;
    return at;
}

/* ---- Reference: independent of the stepped matcher. ---- */
/* One character after `rounds` rounds of JSON string escaping, by recursion on
 * every way a round can spell it. */
static int ref_spell(const char *t, size_t left, char c, int rounds, size_t *used)
{
    char alt[4][8];
    int i, j, n = 0;
    if (!rounds) {
        if (!left || t[0] != c) return 0;
        *used = 1;
        return 1;
    }
    if (c != '"' && c != '\\') { alt[n][0] = c; alt[n][1] = 0; n++; }
    if (c == '"' || c == '\\' || c == '/') { alt[n][0] = '\\'; alt[n][1] = c; alt[n][2] = 0; n++; }
    snprintf(alt[n++], 8, "\\u00%02x", (unsigned)(unsigned char)c);
    snprintf(alt[n++], 8, "\\u00%02X", (unsigned)(unsigned char)c);
    for (i = 0; i < n; i++) {
        size_t at = 0, u = 0;
        for (j = 0; alt[i][j]; j++) {
            if (!ref_spell(t + at, left - at, alt[i][j], rounds - 1, &u)) break;
            at += u;
        }
        if (!alt[i][j]) { *used = at; return 1; }
    }
    return 0;
}
static int ref_at(const char *t, size_t left, const char *needle, int rounds, size_t *total)
{
    size_t used = 0, at = 0;
    for (; *needle; needle++) {
        if (!ref_spell(t + at, left - at, *needle, rounds, &used)) return 0;
        at += used;
    }
    *total = at;
    return 1;
}
static void ref_pass(char *s, size_t len, const char *needle, int rounds, int json)
{
    size_t pos = 0;
    int in = 0;
    if (!*needle) return;
    while (pos < len) {
        size_t total = 0;
        if ((!json || in) && ref_at(s + pos, len - pos, needle, rounds, &total)) {
            memset(s + pos, '*', total);
            pos += total;
            continue;
        }
        if (json && in && s[pos] == '\\') pos += 2;
        else { if (json && s[pos] == '"') in = !in; pos++; }
    }
}
static void ref_redact(char *s, size_t len, const char *session, int json)
{
    const char *needles[SECRET_COUNT + 1];
    int i, count = 0;
    if (session) needles[count++] = session;
    for (i = 0; i < SECRET_COUNT; i++) needles[count++] = secrets[i];
    for (i = 0; i < count; i++) {
        if (!json) ref_pass(s, len, needles[i], 0, 0);
        ref_pass(s, len, needles[i], 1, json);
        if (json) ref_pass(s, len, needles[i], 2, json);
    }
}

/* ---- The plain-text helper. ---- */
static void decoded_reflections(void)
{
    char text[256];
    /* Decoded text, a JSON-escaped copy of it and \u spellings of it. */
    strcpy(text, "a Bearer ab\"c\\d/e b ab\\\"c\\\\d/e c ab\\\"c\\\\d\\/e d ab\\u0022c\\u005Cd\\u002fe e");
    mcp_redact(&config, NULL, text);
    assert(!strstr(text, "ab\"c") && !strstr(text, "ab\\\"c") && !strstr(text, "u0022") && !strstr(text, "u005C"));
    assert(!strcmp(text, "a *************** b ********** c *********** d *********************** e"));
    /* Escaped key and session. */
    strcpy(text, "k9\\u0041zz k\\u0039Azz " SESSION " sess-Q7x\\/9 tail");
    mcp_redact(&config, SESSION, text);
    assert(!strcmp(text, "********** ********** ********** *********** tail"));
    /* Neighbouring text, near misses and non-secret escapes survive. */
    strcpy(text, "k9Azy \\n ab\\\"c\\\\d/f \\u00e9 querysecret");
    mcp_redact(&config, SESSION, text);
    assert(!strcmp(text, "k9Azy \\n ab\\\"c\\\\d/f \\u00e9 querysecret"));
    /* A backslash at the very end of the text is not read past. */
    strcpy(text, "ab\\");
    mcp_redact(&config, NULL, text);
    assert(!strcmp(text, "ab\\"));
}
static void helper_matches_reference(void)
{
    static const char *const pieces[] = {"Bearer ab\"c\\d/e", "ab\"c\\d/e", "ab\\\"c\\\\d/e", "ab\\u0022c\\u005cd\\/e",
        "k9Azz", "k9\\u0041zz", "k\\u0039\\u0041zz", "querysecret1", "quer\\u0079secret1", SESSION, "sess-Q7x\\/9",
        "\\", "\\\\", "\"", "\\u", "\\u00", "x", " ", "*", "\\u0041", "9", "ab\\\\\\\"c\\\\\\\\d/e", "k9\\\\u0041zz", "ab\\\\u0022c"};
    static const size_t budgets[] = {1, 7, 100, 8192};
    char text[600], want[600], got[600];
    int i, k, b, json;
    for (json = 0; json <= 1; json++) for (i = 0; i < 6000; i++) {
        const char *session = rnd(3) ? SESSION : NULL;
        size_t len;
        text[0] = 0;
        for (k = 0; k < 1 + (int)rnd(8); k++) strcat(text, pieces[rnd(sizeof(pieces) / sizeof(pieces[0]))]);
        len = strlen(text);
        strcpy(want, text);
        ref_redact(want, len, session, json);
        for (b = 0; b < 4; b++) {
            McpRedact r;
            int steps = 0;
            strcpy(got, text);
            mcp_redact_init_text(&r, &config, session, got, len, json);
            while (!mcp_redact_step(&r, budgets[b])) {
                assert(r.work <= budgets[b]);
                assert(++steps < 4000000);
            }
            assert(!strcmp(got, want));
        }
        if (!json) { /* the NUL-terminated form is the same thing */
            strcpy(got, text);
            mcp_redact(&config, session, got);
            assert(!strcmp(got, want));
        }
    }
}

/* ---- Schemas and names. ---- */
static const char *run_page(const char *page)
{
    memset(&registry, 0, sizeof(registry));
    mcp_work_init(&sync_work, MCP_STAGE_DISCOVER, &config, &registry, page, strlen(page), 0, NULL, 0, NULL, 0);
    sync_work.session = SESSION;
    sync_run(MCP_WORK_BUDGET);
    return registry.schemas;
}
static void schemas_and_names(void)
{
    static const char page[] = "{\"result\":{\"tools\":["
        "{\"name\":\"search\",\"annotations\":{\"readOnlyHint\":true},"
        "\"description\":\"Send header Bearer ab\\\"c\\\\d/e and {\\\"auth\\\":\\\"ab\\\\\\\"c\\\\\\\\d/e\\\"} session " SESSION " ok\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
        "\"q\":{\"type\":\"string\",\"description\":\"key k9\\u0041zz and ab\\\"c\\\\d\\/e at querysecret1\",\"default\":\"sess-Q7x\\/9\","
        "\"enum\":[\"plain\",\"k9Azz\"]},"
        "\"k9Azz\":{\"type\":\"integer\",\"maximum\":9,\"description\":\"Keeps \\u00e9 and \\n\"}},"
        "\"required\":[\"q\"]}},"
        "{\"name\":\"k9Azz\",\"annotations\":{\"readOnlyHint\":true},\"inputSchema\":{}},"
        "{\"name\":\"fine\",\"annotations\":{\"readOnlyHint\":true},\"inputSchema\":{\"type\":\"object\"}}]}}";
    const char *schemas = run_page(page);
    JsonToken tokens[256];
    assert(sync_work.status == 0);
    /* The tool named by a credential is skipped; the others survive. */
    assert(registry.count == 2 && !strcmp(registry.tools[0].original, "search") && !strcmp(registry.tools[1].original, "fine"));
    assert(strstr(registry.notice, "credential"));
    assert(!strstr(schemas, "k9Azz") && !strstr(schemas, "k9\\u0041zz") && !strstr(schemas, "querysecret1"));
    assert(!strstr(schemas, "ab\\\"c") && !strstr(schemas, "d\\/e") && !strstr(schemas, "sess-Q7x") && !strstr(schemas, "u0022"));
    /* Non-secret content is kept, and the whole list is still valid JSON of the same shape. */
    assert(strstr(schemas, "\"maximum\":9") && strstr(schemas, "\"required\":[\"q\"]") && strstr(schemas, "Keeps \\u00e9 and \\n"));
    assert(strstr(schemas, "\"enum\":[\"plain\",\"*****\"]"));
    assert(strstr(schemas, "\"name\":\"mcp_srv_fine\""));
    {
        char wrapped[MCP_SCHEMA_CAP + 8];
        emit(wrapped, "[%s]", schemas);
        assert(json_parse(wrapped, strlen(wrapped), tokens, 256) > 0);
    }
    /* The length of the schema buffer still describes what it holds. */
    assert(registry.schema_len == strlen(schemas));
    /* A server that sends the same page without our credentials gets it back unchanged. */
    {
        static const char clean[] = "{\"result\":{\"tools\":[{\"name\":\"s\",\"annotations\":{\"readOnlyHint\":true},"
            "\"description\":\"Plain\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"a b\":{\"description\":\"x\\\"y\\\\z\\/\"}}}}]}}";
        schemas = run_page(clean);
        assert(strstr(schemas, "\"properties\":{\"a b\":{\"description\":\"x\\\"y\\\\z\\/\"}}}"));
    }
    /* Structure is not redacted: a credential value that equals a keyword outside strings. */
    {
        static const char numeric[] = "{\"mcpServers\":{\"srv\":{\"url\":\"https://x.test/mcp\",\"headers\":{\"X-Level\":\"12345678\"}}}}";
        static McpConfig other;
        char error[128];
        static const char digits[] = "{\"result\":{\"tools\":[{\"name\":\"n\",\"annotations\":{\"readOnlyHint\":true},"
            "\"inputSchema\":{\"maximum\":12345678,\"enum\":[\"12345678\"]}}]}}";
        assert(!mcp_config_parse(numeric, strlen(numeric), &other, error, sizeof(error)));
        memset(&registry, 0, sizeof(registry));
        mcp_work_init(&sync_work, MCP_STAGE_DISCOVER, &other, &registry, digits, strlen(digits), 0, NULL, 0, NULL, 0);
        sync_run(MCP_WORK_BUDGET);
        assert(strstr(registry.schemas, "\"maximum\":12345678,\"enum\":[\"********\"]"));
    }
}

/* ---- Call results and server errors. ---- */
static int run_call(const char *text, int rewrite, size_t budget)
{
    size_t length = strlen(text);
    memcpy(message, text, length + 1);
    mcp_work_init(&sync_work, MCP_STAGE_CLASSIFY | MCP_STAGE_CALL, &config, &registry, message, length, 5, reply, sizeof(reply), NULL, 0);
    sync_work.session = SESSION;
    if (rewrite) sync_work.rewrite = message;
    sync_run(budget);
    return sync_work.rpc == 1 && sync_work.status == 0 ? 0 : -1;
}
/* Same tokens, same positions: only characters inside strings changed. */
static void same_shape(const char *before, const char *after)
{
    static JsonToken a[8192], b[8192];
    int n = json_parse(before, strlen(before), a, 8192), m = json_parse(after, strlen(after), b, 8192), i;
    assert(n > 0 && n == m && strlen(before) == strlen(after));
    for (i = 0; i < n; i++)
        assert(a[i].type == b[i].type && a[i].start == b[i].start && a[i].end == b[i].end && a[i].next == b[i].next);
}
static void call_results(void)
{
    static const char ok[] = "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"content\":[{\"type\":\"text\","
        "\"text\":\"echo Bearer ab\\\"c\\\\d/e then k9\\u0041zz in sess-Q7x\\/9 and querysecret1; cafe\\u00e9 stays\"}],"
        "\"structuredContent\":{\"headers\":{\"Authorization\":\"Bearer ab\\\"c\\\\d/e\"},\"nested\":\"{\\\"k\\\":\\\"ab\\\\\\\"c\\\\\\\\d/e\\\"}\"},"
        "\"isError\":false}}";
    static const char failed[] = "{\"jsonrpc\":\"2.0\",\"id\":5,\"error\":{\"code\":-32000,"
        "\"message\":\"denied for Bearer ab\\\"c\\\\d/e (session sess-Q7x/9)\",\"data\":{\"key\":\"k9Azz\"}}}";
    static const char flagged[] = "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"isError\":true,"
        "\"content\":[{\"type\":\"text\",\"text\":\"bad key querysecret1\"}]}}";
    size_t budgets[] = {MCP_WORK_BUDGET, 1000, 24};
    int b;
    for (b = 0; b < 3; b++) {
        assert(!run_call(ok, 1, budgets[b]) && !sync_work.call_error);
        same_shape(ok, message);
        assert(strstr(message, "\"id\":5") && strstr(message, "\"jsonrpc\":\"2.0\"") && strstr(message, "cafe\\u00e9 stays"));
        assert(strstr(message, "\"isError\":false") && strstr(message, "\"type\":\"text\""));
        assert(!strstr(message, "ab\\\"c") && !strstr(message, "k9") && !strstr(message, "sess-Q7x") && !strstr(message, "querysecret1"));
        assert(!strstr(message, "d/e") && !strstr(message, "\\\\\\\\d"));
        assert(strstr(message, "\"text\":\"echo ***************** then ********** in *********** and ************; cafe"));

        assert(!run_call(failed, 1, budgets[b]) && sync_work.call_error);
        same_shape(failed, message);
        assert(!strstr(message, "ab\\\"c") && !strstr(message, "sess-Q7x") && !strstr(message, "k9Azz"));
        assert(strstr(message, "\"code\":-32000") && strstr(message, "denied for ") && strstr(message, " (session ") && strstr(message, "\"error\""));

        assert(!run_call(flagged, 1, budgets[b]) && sync_work.call_error);
        same_shape(flagged, message);
        assert(!strstr(message, "querysecret1") && strstr(message, "bad key ") && strstr(message, "\"isError\":true"));
    }
    /* The envelope is not rewritten, and a result that is not an object still is. */
    assert(!run_call("{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":\"k9Azz\"}", 1, MCP_WORK_BUDGET));
    assert(!strcmp(message, "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":\"*****\"}"));
    assert(!run_call("{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":[\"querysecret1\",{\"k9Azz\":1}]}", 1, MCP_WORK_BUDGET));
    assert(!strcmp(message, "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":[\"************\",{\"*****\":1}]}"));
    /* Without the rewrite pointer the message is left as the caller owns it. */
    assert(!run_call(ok, 0, MCP_WORK_BUDGET) && !strcmp(message, ok));
    /* Messages that are not the matching response are never rewritten. */
    assert(run_call("{\"jsonrpc\":\"2.0\",\"id\":9,\"result\":{\"text\":\"k9Azz\"}}", 1, MCP_WORK_BUDGET) < 0);
    assert(strstr(message, "k9Azz"));
}
/* `body` is the inside of a JSON string literal: its decoded text. */
static int decode_body(const char *body, char *out, size_t cap)
{
    static char quoted[4096];
    JsonToken t[2];
    emit(quoted, "\"%s\"", body);
    return json_parse(quoted, strlen(quoted), t, 2) == 1 ? json_string(quoted, t, 0, out, cap) : -1;
}
/* Whatever way a credential is spelled inside the JSON string, none survives
 * decoding: once, or twice for a JSON document carried in the string. */
static void spellings_never_survive(void)
{
    char plain[256], once[2048], twice[4096], text[8192], decoded[2048], inner[2048];
    JsonToken t[64];
    int i, k;
    for (i = 0; i < 4000; i++) {
        int n = 1 + (int)rnd(3), nested = i % 2;
        plain[0] = 0;
        for (k = 0; k < n; k++) {
            strcat(plain, rnd(2) ? "lorem " : "ipsum-");
            strcat(plain, rnd(4) ? secrets[rnd(SECRET_COUNT)] : SESSION);
            strcat(plain, rnd(2) ? " " : "\\");
        }
        encode(once, plain);
        if (nested) encode(twice, once);
        emit(text, "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"content\":[{\"text\":\"%s\"}]}}", nested ? twice : once);
        assert(!run_call(text, 1, i % 4 ? MCP_WORK_BUDGET : 50));
        same_shape(text, message);
        assert(json_parse(message, strlen(message), t, 64) > 0);
        k = json_member(message, t, json_member(message, t, 0, "result"), "content");
        k = json_member(message, t, k + 1, "text");
        assert(k > 0 && json_string(message, t, k, decoded, sizeof(decoded)) >= 0);
        if (nested) {
            /* The redacted body may end inside the inner document; it still decodes. */
            assert(decode_body(decoded, inner, sizeof(inner)) >= 0);
            memcpy(decoded, inner, strlen(inner) + 1);
        }
        for (k = 0; k < SECRET_COUNT; k++) assert(!strstr(decoded, secrets[k]));
        assert(!strstr(decoded, SESSION));
        assert(strstr(decoded, "lorem") || strstr(decoded, "ipsum"));
    }
}
/* A message at the cap, dense with credentials: every step stays in budget. */
static void bounded(void)
{
    static const char head[] = "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"";
    size_t at = emit(message, "%s", head), end;
    while (at < MCP_MESSAGE_CAP - 100) at += emit(message + at, "x Bearer ab\\\"c\\\\d/e %s k9Azz querysecret1 ", SESSION);
    at += emit(message + at, "\"}]}}");
    end = at;
    memcpy(copy, message, end + 1);
    assert(!run_call(copy, 1, MCP_WORK_BUDGET));
    assert(end <= MCP_MESSAGE_CAP && sync_work.peak <= MCP_WORK_BUDGET);
    assert(!strstr(message, "k9Azz") && !strstr(message, "querysecret1") && !strstr(message, "ab\\\"c") && !strstr(message, "sess-Q7x"));
    same_shape(copy, message);
    /* The cost is the message size times the needles and forms: bounded, and counted. */
    assert(sync_work.steps > end / MCP_WORK_BUDGET && sync_work.units < end * 40);
}

int main(void)
{
    char error[128];
    assert(!mcp_config_parse(configuration, strlen(configuration), &config, error, sizeof(error)));
    assert(strstr(config.headers, "Authorization: Bearer ab\"c\\d/e\r\n"));
    decoded_reflections();
    helper_matches_reference();
    schemas_and_names();
    call_results();
    spellings_never_survive();
    bounded();
    puts("PASS MCP credential redaction: decoded and escaped reflections, schemas, names, call results, errors and session IDs");
    return 0;
}
