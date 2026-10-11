/* Retained MCP results against a File Manager model with fault injection:
 * every staging, verification, publication and journal failure must leave
 * no advertised artifact, and Stop must abandon a partial retention. Pages
 * must reconstruct the decoded artifact exactly, across multi-byte UTF-8. */
#include "mcp_result.h"
#include "json.h"
#include <Files.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FOLDER "Retro68:Sherclawk MCP Results:"
#define ARTIFACT_TYPE 0x55544638UL

static struct File {
    int used, dir;
    char name[320];
    char bytes[70000];
    long size;
    unsigned long type;
} files[24];
static long position[24];
static int open_refs[24]; /* open forks: a busy file cannot be deleted */
static int creates, opens, writes, reads, renames, flushes, dir_creates, deletes, closes, setposs;
static int fail_dir_create, fail_create_at, fail_open_at, fail_write_at, short_write;
static int fail_close_at, fail_flush_at, fail_read_at, corrupt_read_at, fail_rename_at;
static int fail_setpos_at, journal_fail;
static unsigned long ticks = 0x2a;
static int open_fork_count(void)
{
    int i, total = 0;
    for (i = 0; i < 24; i++) total += open_refs[i];
    return total;
}

unsigned long TickCount(void) { return ticks; }

static int find_name(const char *name)
{
    int i;
    for (i = 0; i < 24; i++) if (files[i].used && !strcmp(files[i].name, name)) return i;
    return -1;
}
static int find(const unsigned char *p)
{
    char name[320];
    memcpy(name, p + 1, p[0]); name[p[0]] = 0;
    return find_name(name);
}
static int add(const char *name, int dir, unsigned long type)
{
    int i;
    for (i = 0; i < 24; i++) if (!files[i].used) {
        memset(&files[i], 0, sizeof(files[i]));
        files[i].used = 1; files[i].dir = dir; files[i].type = type;
        strcpy(files[i].name, name);
        return i;
    }
    abort();
}
OSErr FSMakeFSSpec(short vol, long parent, const unsigned char *name, FSSpec *spec)
{
    memset(spec, 0, sizeof(*spec));
    spec->vRefNum = vol; spec->parID = parent;
    memcpy(spec->name, name, (size_t)name[0] + 1);
    return find(name) < 0 ? fnfErr : noErr;
}
OSErr FSpDirCreate(const FSSpec *s, short code, long *id)
{
    char name[320]; (void)code;
    if (++dir_creates == fail_dir_create) return ioErr;
    memcpy(name, s->name + 1, s->name[0]); name[s->name[0]] = 0;
    add(name, 1, 0); *id = 3;
    return noErr;
}
OSErr FSpCreate(const FSSpec *s, unsigned long creator, unsigned long type, short code)
{
    char name[320]; (void)creator; (void)code;
    if (++creates == fail_create_at) return ioErr;
    if (find(s->name) >= 0) return dupFNErr;
    memcpy(name, s->name + 1, s->name[0]); name[s->name[0]] = 0;
    add(name, 0, type);
    return noErr;
}
OSErr FSpOpenDF(const FSSpec *s, short mode, short *ref)
{
    int i = find(s->name); (void)mode;
    if (i < 0) return fnfErr;
    if (files[i].dir) return paramErr;
    if (++opens == fail_open_at) return ioErr;
    *ref = (short)i; position[i] = 0; open_refs[i]++;
    return noErr;
}
OSErr FSWrite(short ref, long *n, const void *bytes)
{
    if (++writes == fail_write_at) { *n = 0; return ioErr; }
    if (short_write && *n) --*n;
    assert(position[ref] + *n <= (long)sizeof(files[ref].bytes));
    memcpy(files[ref].bytes + position[ref], bytes, (size_t)*n);
    position[ref] += *n;
    if (position[ref] > files[ref].size) files[ref].size = position[ref];
    return noErr;
}
OSErr FSRead(short ref, long *n, void *bytes)
{
    if (++reads == fail_read_at) { *n = 0; return ioErr; }
    if (*n > files[ref].size - position[ref]) *n = files[ref].size - position[ref];
    memcpy(bytes, files[ref].bytes + position[ref], (size_t)*n);
    position[ref] += *n;
    if (reads == corrupt_read_at && *n) ((char *)bytes)[0] ^= 1;
    return noErr;
}
OSErr GetEOF(short ref, long *eof) { *eof = files[ref].size; return noErr; }
OSErr SetFPos(short ref, short pos, long off)
{
    (void)pos;
    if (++setposs == fail_setpos_at) return ioErr;
    position[ref] = off;
    return noErr;
}
OSErr FSClose(short ref)
{
    assert(open_refs[ref] > 0);
    open_refs[ref]--; /* a failed close still releases the fork */
    if (++closes == fail_close_at) return ioErr;
    return noErr;
}
OSErr FlushVol(const unsigned char *name, short vol)
{
    (void)name; (void)vol;
    if (++flushes == fail_flush_at) return ioErr;
    return noErr;
}
OSErr FSpDelete(const FSSpec *s)
{
    int i = find(s->name);
    deletes++;
    if (i < 0) return fnfErr;
    if (open_refs[i] > 0) return fBsyErr;
    files[i].used = 0;
    return noErr;
}
OSErr FSpRename(const FSSpec *s, const unsigned char *to)
{
    char name[320], *colon;
    int i = find(s->name);
    if (++renames == fail_rename_at) return ioErr;
    if (i < 0) return fnfErr;
    memcpy(name, s->name + 1, s->name[0]); name[s->name[0]] = 0;
    colon = strrchr(name, ':');
    if (!colon) return paramErr;
    colon[1] = 0;
    strncat(name, (const char *)to + 1, (size_t)to[0]);
    if (find_name(name) >= 0) return dupFNErr;
    memset(files[i].name, 0, sizeof(files[i].name));
    strcpy(files[i].name, name);
    return noErr;
}
OSErr PBGetCatInfoSync(CInfoPBRec *pb)
{
    int i = find(pb->hFileInfo.ioNamePtr);
    if (i < 0) return fnfErr;
    pb->hFileInfo.ioFlAttrib = (unsigned char)(files[i].dir ? 16 : 0);
    pb->hFileInfo.ioFlFndrInfo.fdFlags = 0;
    pb->hFileInfo.ioFlFndrInfo.fdType = files[i].type;
    pb->hFileInfo.ioFlLgLen = files[i].size;
    pb->hFileInfo.ioFlRLgLen = 0;
    return noErr;
}

/* ---- Journal stub and request helpers. ---- */
static int journal_calls;
static char last_event[64], last_body[512];
static int journal(void *context, const char *event, const char *json)
{
    (void)context;
    if (journal_fail) return -1;
    journal_calls++;
    snprintf(last_event, sizeof(last_event), "%s", event);
    snprintf(last_body, sizeof(last_body), "%s", json);
    return 0;
}
static McpConfig config;
static char config_error[256];
static char summary[AGENT_RESULT_CAP], read_result[AGENT_RESULT_CAP];
static AgentCall call;
static int fault_journal;

static void reset(void)
{
    memset(files, 0, sizeof(files));
    memset(open_refs, 0, sizeof(open_refs));
    mcp_result_new_session(); /* fresh session: empty table, stale old IDs */
    creates = opens = writes = reads = renames = flushes = dir_creates = deletes = closes = setposs = 0;
    fail_dir_create = fail_create_at = fail_open_at = fail_write_at = short_write = 0;
    fail_close_at = fail_flush_at = fail_read_at = corrupt_read_at = fail_rename_at = 0;
    fail_setpos_at = journal_fail = 0;
    journal_calls = 0; fault_journal = 0;
    last_event[0] = last_body[0] = 0;
    memset(&config, 0, sizeof(config));
    memset(&call, 0, sizeof(call));
}

/* Pump a retention to its terminal state; returns the terminal code. */
static int retain(const char *response, const char *folder, const McpConfig *cfg,
                  const char *session)
{
    McpResultRetain in;
    int r, guard = 0;
    memset(&in, 0, sizeof(in));
    in.response = response;
    in.response_len = strlen(response);
    in.folder = folder;
    in.config = cfg;
    in.session = session;
    in.journal = fault_journal ? NULL : journal;
    r = mcp_result_retain_begin(&in, summary, sizeof(summary));
    while (r == 2) {
        assert(++guard < 100000);
        r = mcp_result_retain_step(summary, sizeof(summary), 0);
    }
    return r;
}
static int read_page(const char *id, long start)
{
    char args[160];
    snprintf(args, sizeof(args), "{\"result_id\":\"%s\",\"start_byte\":%ld}", id, start);
    memset(&call, 0, sizeof(call));
    strcpy(call.name, "read_mcp_result");
    strcpy(call.arguments, args);
    return mcp_result_read(&call, read_result, sizeof(read_result));
}
/* Field helpers over the JSON envelopes. */
static char field_text[AGENT_RESULT_CAP];
static const char *field_str(const char *body, const char *key)
{
    JsonToken t[64];
    assert(json_parse(body, strlen(body), t, 64) > 1);
    assert(json_string(body, t, json_member(body, t, 0, key), field_text, sizeof(field_text)) >= 0);
    return field_text;
}
static long field_long(const char *body, const char *key)
{
    JsonToken t[64];
    long value = -1;
    assert(json_parse(body, strlen(body), t, 64) > 1);
    assert(!json_integer(body, t, json_member(body, t, 0, key), &value));
    return value;
}
static int field_true(const char *body, const char *key)
{
    JsonToken t[64];
    int at;
    assert(json_parse(body, strlen(body), t, 64) > 1);
    at = json_member(body, t, 0, key);
    return at > 0 && t[at].type == JSON_PRIMITIVE && t[at].end - t[at].start == 4 &&
        !strncmp(body + t[at].start, "true", 4);
}
/* Follow next_byte until truncated is false, collecting decoded text. */
static void reconstruct(const char *id, char *out, size_t cap)
{
    long start = 0;
    size_t at = 0;
    const char *text;
    for (;;) {
        assert(read_page(id, start) == 0);
        assert(strstr(read_result, "\"status\":\"ok\""));
        text = field_str(read_result, "text");
        assert(strlen(text) < cap - at);
        memcpy(out + at, text, strlen(text) + 1);
        at += strlen(text);
        if (!field_true(read_result, "truncated")) break;
        start = field_long(read_result, "next_byte");
    }
}
static int artifact_count(void)
{
    int i, n = 0;
    for (i = 0; i < 24; i++)
        if (files[i].used && !files[i].dir && !strncmp(files[i].name, FOLDER, strlen(FOLDER))) n++;
    return n;
}
static int artifact_count_suffix(const char *suffix)
{
    int i, n = 0;
    size_t k = strlen(FOLDER), s = strlen(suffix);
    for (i = 0; i < 24; i++)
        if (files[i].used && !files[i].dir && !strncmp(files[i].name, FOLDER, k) &&
            !strcmp(files[i].name + strlen(files[i].name) - s, suffix)) n++;
    return n;
}

/* ---- Fixtures. ---- */
static const char two_items[] =
    "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"content\":["
    "{\"type\":\"text\",\"text\":\"First https://tavily.com/a result line\"},"
    "{\"type\":\"text\",\"text\":\"Second \\u00e9 with https://example.org/b?x=1 link\"}"
    "]}}";
static const char error_response[] =
    "{\"jsonrpc\":\"2.0\",\"id\":6,\"error\":{\"code\":-32000,\"message\":\"boom\\nno access\"}}";
static const char is_error_response[] =
    "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"isError\":true,\"content\":"
    "[{\"type\":\"text\",\"text\":\"remote tool failed\"}]}}";
static const char unsupported_response[] =
    "{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":{\"content\":["
    "{\"type\":\"text\",\"text\":\"the only text\"},"
    "{\"type\":\"image\",\"data\":\"...\",\"mimeType\":\"image/png\"},"
    "{\"type\":\"resource_link\",\"uri\":\"https://example.test/r\",\"name\":\"r\"},"
    "{\"type\":\"custom\",\"x\":1},"
    "42"
    "]}}";

static void test_retain_and_read(void)
{
    char id[64], text[4096], again[4096], args[96];
    const char *expected = "First https://tavily.com/a result line"
                           "\nSecond \xc3\xa9 with https://example.org/b?x=1 link";
    long bytes = (long)strlen(expected);
    reset();
    assert(retain(two_items, FOLDER, NULL, NULL) == 0);
    assert(strstr(summary, "\"status\":\"ok\"") && strstr(summary, "\"content_type\":\"text\""));
    snprintf(id, sizeof(id), "%s", field_str(summary, "result_id"));
    assert(strlen(id) == 17 && !strncmp(id, "mcp-", 4));
    assert(field_long(summary, "bytes") == bytes);
    assert(field_long(summary, "items") == 2);
    assert(field_long(summary, "unsupported") == 0);
    assert(!field_true(summary, "is_error"));
    assert(strstr(summary, "https://tavily.com/a") && strstr(summary, "https://example.org/b?x=1"));
    assert(strstr(summary, "\"links\":[\"https://tavily.com/a\",\"https://example.org/b?x=1\"]"));
    /* The artifact exists once, published, with the right type and size. */
    assert(artifact_count() == 1 && artifact_count_suffix(".new") == 0);
    {
        char path[320];
        int i;
        snprintf(path, sizeof(path), "%s%s", FOLDER, id);
        i = find_name(path);
        assert(i >= 0 && files[i].size == bytes && files[i].type == ARTIFACT_TYPE);
    }
    assert(journal_calls == 1 && !strcmp(last_event, "mcp_result_retained"));
    assert(strstr(last_body, id));
    /* Pages reconstruct the decoded artifact exactly, and are stable. */
    reconstruct(id, text, sizeof(text));
    assert(!strcmp(text, expected));
    assert(read_page(id, 0) == 0);
    snprintf(again, sizeof(again), "%s", read_result);
    assert(read_page(id, 0) == 0);
    assert(!strcmp(again, read_result));
    /* The end of the artifact reads empty; past it is a range error. */
    assert(read_page(id, bytes) == 0);
    assert(field_long(read_result, "start_byte") == bytes && !strcmp(field_str(read_result, "text"), ""));
    assert(read_page(id, bytes + 1) == 0);
    assert(strstr(read_result, "\"code\":\"RANGE\""));
    /* Unknown and malformed IDs are stale in this session. */
    assert(read_page("mcp-00000000-0000", 0) == 0);
    assert(strstr(read_result, "\"code\":\"STALE_ID\""));
    snprintf(args, sizeof(args), "{\"result_id\":\"%s\",\"start_byte\":-1}", id);
    memset(&call, 0, sizeof(call)); strcpy(call.name, "read_mcp_result"); strcpy(call.arguments, args);
    assert(mcp_result_read(&call, read_result, sizeof(read_result)) == 0);
    assert(strstr(read_result, "\"code\":\"ARGUMENTS\""));
}
/* A response whose text needs several 8 KiB staging chunks, so intermediate
 * states are observable between steps. */
static char many_a[20001];
static char big_response[21000];
static void build_big(void)
{
    memset(many_a, 'a', sizeof(many_a) - 1);
    snprintf(big_response, sizeof(big_response),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"%.*s\"}]}}",
        20000, many_a);
}
static void test_utf8_boundaries(void)
{
    static char response[1024], id[64], text[8192], expected[4096];
    reset();
    /* A two-byte character cut by the page cap stays whole. */
    memset(expected, 'a', 511);
    memcpy(expected + 511, "\xe2\x82\xac" "tail", 7);
    expected[511 + 7] = 0;
    snprintf(response, sizeof(response),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":"
        "\"%.*s\\u20actail\"}]}}", 511, expected);
    assert(retain(response, FOLDER, NULL, NULL) == 0);
    snprintf(id, sizeof(id), "%s", field_str(summary, "result_id"));
    assert(field_long(summary, "bytes") == 518);
    reconstruct(id, text, sizeof(text));
    assert(!strcmp(text, expected));
    /* A start_byte inside the euro is a range error, not corrupt data. */
    assert(read_page(id, 512) == 0);
    assert(strstr(read_result, "\"code\":\"RANGE\""));
    /* The first page carried the whole euro: next_byte is past its last byte. */
    assert(read_page(id, 0) == 0);
    assert(field_long(read_result, "next_byte") >= 514);
    assert(field_long(read_result, "next_byte") <= MCP_RESULT_PAGE + 3);
    /* A four-byte character straddling the cap stays whole too. */
    reset();
    memset(expected, 0, sizeof(expected));
    memset(expected, 'a', 510);
    memcpy(expected + 510, "\xf0\x9f\x98\x80" "end", 7);
    expected[510 + 7] = 0;
    snprintf(response, sizeof(response),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":"
        "\"%.*s\\ud83d\\ude00end\"}]}}", 510, expected);
    assert(retain(response, FOLDER, NULL, NULL) == 0);
    snprintf(id, sizeof(id), "%s", field_str(summary, "result_id"));
    reconstruct(id, text, sizeof(text));
    assert(!strcmp(text, expected));
    /* Every page re-read at the same offset is identical. */
    {
        long start = 0;
        while (1) {
            char first[AGENT_RESULT_CAP];
            assert(read_page(id, start) == 0);
            memcpy(first, read_result, sizeof(first));
            assert(read_page(id, start) == 0);
            assert(!strcmp(first, read_result));
            if (!field_true(read_result, "truncated")) break;
            start = field_long(read_result, "next_byte");
        }
    }
}
static void test_error_and_unsupported(void)
{
    char id[64], text[4096];
    reset();
    assert(retain(error_response, FOLDER, NULL, NULL) == 0);
    assert(field_true(summary, "is_error"));
    assert(!strcmp(field_str(summary, "error_code"), "-32000"));
    assert(field_long(summary, "items") == 0 && field_long(summary, "unsupported") == 0);
    snprintf(id, sizeof(id), "%s", field_str(summary, "result_id"));
    assert(field_long(summary, "bytes") == 14);
    reconstruct(id, text, sizeof(text));
    assert(!strcmp(text, "boom\nno access"));
    assert(read_page(id, 0) == 0 && strstr(read_result, "\"is_error\":true"));
    /* A remote tool error still retains its text. */
    reset();
    assert(retain(is_error_response, FOLDER, NULL, NULL) == 0);
    assert(field_true(summary, "is_error"));
    assert(field_long(summary, "items") == 1);
    snprintf(id, sizeof(id), "%s", field_str(summary, "result_id"));
    reconstruct(id, text, sizeof(text));
    assert(!strcmp(text, "remote tool failed"));
    /* An error without a message retains nothing and no artifact appears. */
    reset();
    assert(retain("{\"jsonrpc\":\"2.0\",\"id\":6,\"error\":{\"code\":-1}}", FOLDER, NULL, NULL) == 0);
    assert(!strstr(summary, "result_id") && field_long(summary, "bytes") == 0);
    assert(field_true(summary, "is_error") && artifact_count() == 0);
    /* Unsupported content is counted, never retained, and the text items
     * still are. */
    reset();
    assert(retain(unsupported_response, FOLDER, NULL, NULL) == 0);
    assert(field_long(summary, "items") == 1 && field_long(summary, "unsupported") == 4);
    assert(field_long(summary, "bytes") == 13);
    snprintf(id, sizeof(id), "%s", field_str(summary, "result_id"));
    reconstruct(id, text, sizeof(text));
    assert(!strcmp(text, "the only text"));
    /* Malformed responses are refused outright. */
    reset();
    assert(retain("{\"jsonrpc\":\"2.0\",\"id\":1}", FOLDER, NULL, NULL) == 1);
    assert(strstr(summary, "\"code\":\"RETENTION\"") && artifact_count() == 0);
    assert(retain("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{},\"error\":{\"code\":1}}", FOLDER, NULL, NULL) == 1);
    assert(strstr(summary, "\"code\":\"RETENTION\""));
}
static void test_stale_sessions(void)
{
    char id[64];
    reset();
    assert(retain(two_items, FOLDER, NULL, NULL) == 0);
    snprintf(id, sizeof(id), "%s", field_str(summary, "result_id"));
    assert(read_page(id, 0) == 0 && strstr(read_result, "\"status\":\"ok\""));
    /* A new session invalidates every ID, even with the journal records of
     * the old one still on disk: nothing is ever restored from them. */
    mcp_result_new_session();
    assert(read_page(id, 0) == 0 && strstr(read_result, "\"code\":\"STALE_ID\""));
    /* The table is fresh: the same count of results fits again. */
    assert(retain(two_items, FOLDER, NULL, NULL) == 0);
    assert(strcmp(field_str(summary, "result_id"), id));
    /* A session change during retention abandons it: the stage is cleaned up
     * and nothing is advertised. */
    {
        McpResultRetain in;
        int r, guard = 0;
        memset(&in, 0, sizeof(in));
        in.response = two_items; in.response_len = strlen(two_items); in.folder = FOLDER;
        assert(mcp_result_retain_begin(&in, summary, sizeof(summary)) == 2);
        r = 2;
        while (r == 2) {
            assert(++guard < 1000);
            r = mcp_result_retain_step(summary, sizeof(summary), 0);
            if (guard == 1) mcp_result_new_session();
        }
        assert(r == 1 && strstr(summary, "\"code\":\"SESSION\""));
        /* Two retained artifacts remain as evidence; the abandoned one left nothing. */
        assert(artifact_count() == 2 && artifact_count_suffix(".new") == 0);
    }
}
static void test_table_cap(void)
{
    char response[256];
    int i;
    reset();
    for (i = 0; i < MCP_RESULT_MAX; i++) {
        snprintf(response, sizeof(response),
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"r%d\"}]}}", i);
        assert(retain(response, FOLDER, NULL, NULL) == 0);
    }
    assert(artifact_count() == MCP_RESULT_MAX);
    assert(retain("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"x\"}]}}",
                  FOLDER, NULL, NULL) == 0);
    assert(strstr(summary, "\"code\":\"LIMIT\""));
    assert(artifact_count() == MCP_RESULT_MAX);
}
static void test_stop(void)
{
    McpResultRetain in;
    int r, guard = 0;
    build_big();
    reset();
    memset(&in, 0, sizeof(in));
    in.response = big_response; in.response_len = strlen(big_response); in.folder = FOLDER;
    assert(mcp_result_retain_begin(&in, summary, sizeof(summary)) == 2);
    /* Stop before anything reaches the disk. */
    assert(mcp_result_retain_step(summary, sizeof(summary), 1) == 1);
    assert(strstr(summary, "\"code\":\"STOPPED\""));
    assert(artifact_count() == 0);
    assert(mcp_result_retain_step(summary, sizeof(summary), 0) == 1);
    assert(strstr(summary, "\"code\":\"RETAIN_BUSY\""));
    /* Stop with a stage partly written: the stage is cleaned up and the
     * ID is never advertised. The call is not replayed. */
    reset();
    memset(&in, 0, sizeof(in));
    in.response = big_response; in.response_len = strlen(big_response); in.folder = FOLDER;
    assert(mcp_result_retain_begin(&in, summary, sizeof(summary)) == 2);
    r = 2;
    while (r == 2 && artifact_count_suffix(".new") == 0) {
        assert(++guard < 1000);
        r = mcp_result_retain_step(summary, sizeof(summary), 0);
    }
    assert(r == 2);
    assert(mcp_result_retain_step(summary, sizeof(summary), 1) == 1);
    assert(strstr(summary, "\"code\":\"STOPPED\""));
    assert(artifact_count() == 0 && artifact_count_suffix(".new") == 0);
    assert(open_fork_count() == 0);
    /* A stopped retention leaves the adapter reusable. */
    assert(retain(two_items, FOLDER, NULL, NULL) == 0);
    assert(artifact_count() == 1);
}
static void test_faults(void)
{
    char id[64];
    int knob;
    static const char *const codes[] = { "STAGE", "STAGE", "STAGE", "STAGE", "VERIFY",
                                         "VERIFY", "STAGE", "RETENTION", "FOLDER" };
    build_big();    for (knob = 0; knob < 9; knob++) {
        reset();
        if (knob == 0) fail_create_at = 1;
        else if (knob == 1) fail_open_at = 1;
        else if (knob == 2) short_write = 1;
        else if (knob == 3) fail_flush_at = 1;
        else if (knob == 4) fail_read_at = 1;
        else if (knob == 5) corrupt_read_at = 1;
        else if (knob == 6) fail_close_at = 1;
        else if (knob == 7) fail_rename_at = 1;
        else fail_dir_create = 1;
        assert(retain(big_response, FOLDER, NULL, NULL) == (knob == 8 ? 0 : 1));
        assert(strstr(summary, codes[knob]));
        /* No stage remains and nothing was advertised. */
        assert(artifact_count_suffix(".new") == 0);
        assert(artifact_count() == 0);
        assert(open_fork_count() == 0);
        /* The adapter is reusable after every failure. */
        reset();
        assert(retain(two_items, FOLDER, NULL, NULL) == 0);
        snprintf(id, sizeof(id), "%s", field_str(summary, "result_id"));
        assert(read_page(id, 0) == 0 && strstr(read_result, "\"status\":\"ok\""));
    }
    /* A journal failure after publication keeps the artifact as evidence but
     * never advertises the ID. */
    reset();
    journal_fail = 1;
    assert(retain(two_items, FOLDER, NULL, NULL) == 1);
    assert(strstr(summary, "\"code\":\"JOURNAL\""));
    assert(artifact_count() == 1 && artifact_count_suffix(".new") == 0);
    {
        char path[320];
        int i;
        for (i = 0; i < 24; i++)
            if (files[i].used && !files[i].dir && !strncmp(files[i].name, FOLDER, strlen(FOLDER))) {
                snprintf(path, sizeof(path), "%s", files[i].name);
                break;
            }
        assert(i < 24);
        /* The published file is unreachable: its ID never entered the table. */
        assert(read_page(path + strlen(FOLDER), 0) == 0);
        assert(strstr(read_result, "\"code\":\"STALE_ID\""));
    }
    /* An unverified artifact is never advertised mid-flight. */
    reset();
    {
        McpResultRetain in;
        int r, guard = 0, i;
        char id2[64];
        memset(&in, 0, sizeof(in));
        in.response = big_response; in.response_len = strlen(big_response); in.folder = FOLDER;
        assert(mcp_result_retain_begin(&in, summary, sizeof(summary)) == 2);
        r = 2;
        while (r == 2 && artifact_count_suffix(".new") == 0) {
            assert(++guard < 1000);
            r = mcp_result_retain_step(summary, sizeof(summary), 0);
        }
        for (i = 0; i < 24; i++)
            if (files[i].used && !files[i].dir && !strncmp(files[i].name, FOLDER, strlen(FOLDER))) {
                snprintf(id2, sizeof(id2), "%s", files[i].name + strlen(FOLDER));
                id2[strlen(id2) - 4] = 0; /* strip ".new" */
                break;
            }
        assert(i < 24 && r == 2);
        assert(read_page(id2, 0) == 0 && strstr(read_result, "\"code\":\"STALE_ID\""));
        while (r == 2) r = mcp_result_retain_step(summary, sizeof(summary), 0);
        assert(r == 0 && artifact_count() == 1);
        assert(read_page(id2, 0) == 0 && strstr(read_result, "\"status\":\"ok\""));
    }
}
static void test_redaction(void)
{
    static const char configuration[] =
        "{\"mcpServers\":{\"srv\":{\"url\":\"https://x.test/mcp?token=querysecret1\","
        "\"headers\":{\"Authorization\":\"Bearer private-key\"}}}}";
    static const char response[] =
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"result\":{\"content\":[{\"type\":\"text\",\"text\":"
        "\"echo private-key and Bearer private-key and querysecret1 "
        "see https://x.test/p?token=querysecret1 and sess-Q7x/9\"}]}}";
    char id[64], text[4096];
    reset();
    assert(!mcp_config_parse(configuration, strlen(configuration), &config, config_error, sizeof(config_error)));
    assert(retain(response, FOLDER, &config, "sess-Q7x/9") == 0);
    snprintf(id, sizeof(id), "%s", field_str(summary, "result_id"));
    reconstruct(id, text, sizeof(text));
    /* Every credential spelling was redacted before the text was retained. */
    assert(!strstr(text, "private-key") && !strstr(text, "querysecret1") && !strstr(text, "sess-Q7x/9"));
    assert(strstr(text, "****"));
    assert(!strstr(summary, "private-key") && !strstr(summary, "querysecret1"));
}
static void test_retain_arguments(void)
{
    McpResultRetain in;
    reset();
    /* A second retention while one is active is refused. */
    memset(&in, 0, sizeof(in));
    in.response = two_items; in.response_len = strlen(two_items); in.folder = FOLDER;
    assert(mcp_result_retain_begin(&in, summary, sizeof(summary)) == 2);
    assert(mcp_result_retain_begin(&in, summary, sizeof(summary)) == 1);
    assert(strstr(summary, "\"code\":\"RETAIN_BUSY\""));
    assert(mcp_result_retain_step(summary, sizeof(summary), 1) == 1);
    /* Bad requests are refused without touching the disk. */
    in.folder = "Retro68:no-colon";
    assert(mcp_result_retain_begin(&in, summary, sizeof(summary)) == 0);
    assert(strstr(summary, "\"code\":\"ARGUMENTS\""));
    in.folder = FOLDER;
    in.response = ""; in.response_len = 0;
    assert(mcp_result_retain_begin(&in, summary, sizeof(summary)) == 0);
    assert(strstr(summary, "\"code\":\"ARGUMENTS\"") && artifact_count() == 0);
    /* A folder path that would not fit a Pascal string is refused, never truncated. */
    {
        static char long_folder[300];
        int before = dir_creates;
        memset(long_folder, 'f', sizeof(long_folder));
        memcpy(long_folder, "Retro68:", 8);
        long_folder[240] = ':'; long_folder[241] = 0;
        in.folder = long_folder;
        in.response = two_items; in.response_len = strlen(two_items);
        assert(mcp_result_retain_begin(&in, summary, sizeof(summary)) == 0);
        assert(strstr(summary, "\"code\":\"ARGUMENTS\"") && artifact_count() == 0 && dir_creates == before);
    }
    in.folder = FOLDER;
    /* Without a journal configured (diagnostics) retention still works. */
    fault_journal = 1;
    assert(retain(two_items, FOLDER, NULL, NULL) == 0);
    assert(journal_calls == 0 && artifact_count() == 1);
}
int main(void)
{
    test_retain_and_read();
    test_utf8_boundaries();
    test_error_and_unsupported();
    test_stale_sessions();
    test_table_cap();
    test_stop();
    test_faults();
    test_redaction();
    test_retain_arguments();
    puts("PASS mcp_result: retained results, fault fixtures, paging and sessions");
    return 0;
}
