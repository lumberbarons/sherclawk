/* Integration fixture for the MCP run choreography main.c drives: parse the
 * saved configuration, discover eligible tools, freeze the admitted schemas
 * into the model advertisement, dispatch a model-selected remote call with
 * its arguments verbatim, retain the redacted response and page it back. No
 * API key; the fake TLS serves independently authored fixtures and the File
 * Manager model lives in test_mcp_run_fm.c. */
#include "mcp_client.h"
#include "mcp_result.h"
#include "agent.h"
#include "json.h"
#include "chat.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* tools.h pulls <Files.h>, which this unit cannot also include beside the
 * host TLS shim's <OpenTransport.h> (both define noErr); the pure registry
 * counter is all the fixture needs from it. */
extern size_t tools_count(void);

/* ---- Shared File Manager model (test_mcp_run_fm.c) ---- */
void mcp_run_reset(void);
int mcp_run_file_count(void);
int mcp_run_file_count_suffix(const char *suffix);
long mcp_run_file_bytes(int index);

/* ---- Fake TLS (same shape as tests/test_mcp_client.c) ---- */
struct MacTLS_Context {
    char request[12000], response[70000];
    size_t sent, received;
    int used, pumps;
    MacTLS_State state;
};
static struct MacTLS_Context contexts[2];
static McpClient client;
static McpConfig config;
static int creates, closes, lists, calls, cancels;
static int bad_version, reflect;
static char error[256];

MacTLS_Context *MacTLS_Create(const char *host, uint16_t port)
{
    int i;
    assert(!strcmp(host, "mcp.tavily.com") && port == 443);
    for (i = 0; i < 2; i++) if (!contexts[i].used) {
        memset(&contexts[i], 0, sizeof(contexts[i])); contexts[i].used = 1;
        contexts[i].state = kMacTLS_Connected;
        creates++; return &contexts[i];
    }
    assert(0); return NULL;
}
MacTLS_State MacTLS_Pump(MacTLS_Context *c)
{
    assert(c->used); c->pumps++;
    return c->state = kMacTLS_Connected;
}
int MacTLS_Write(MacTLS_Context *c, const void *bytes, size_t len)
{
    assert(c->used && len <= 2048);
    if (len > 7) len = 7; /* backpressure; only a prefix is accepted */
    assert(c->sent + len < sizeof(c->request));
    memcpy(c->request + c->sent, bytes, len); c->sent += len; c->request[c->sent] = 0;
    return (int)len;
}
/* The search text: long enough to page (>512 bytes), with source links, a
 * reflected credential and a reflected session ID that must be redacted, and
 * JSON-escaped UTF-8 that must decode whole across page boundaries. */
static void search_text(char *out, size_t cap)
{
    size_t at = (size_t)snprintf(out, cap,
        "Notes for classic Mac OS 9: see https://example.com/mac-os-9 and "
        "https://example.com/retro68-notes for era details. ");
    int i;
    for (i = 0; i < 16; i++)
        at += (size_t)snprintf(out + at, cap - at, "Paragraph %d caf\\u00e9 repeats across paging boundaries. ", i);
    at += (size_t)snprintf(out + at, cap - at,
        reflect ? "echo Bearer client-secret-key and client-secret-key in opaque-session; kept caf\\u00e9."
                : "End of fixture result.");
    assert(at > 512 && at < cap);
}
static void respond(MacTLS_Context *c)
{
    char *body = strstr(c->request, "\r\n\r\n"), *length = strstr(c->request, "Content-Length: ");
    long wanted;
    int scanned;
    static char reply[66000], text[2048];
    if (!body || !length) return;
    scanned = sscanf(length, "Content-Length: %ld", &wanted);
    assert(scanned == 1);
    body += 4;
    if ((long)strlen(body) < wanted) return;
    assert((long)strlen(body) == wanted);
    if (strstr(body, "\"method\":\"initialize\"")) {
        snprintf(reply, sizeof(reply), "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"%s\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"fixture\",\"version\":\"1\"}}}", bad_version ? "2024-11-05" : "2025-11-25");
        snprintf(c->response, sizeof(c->response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nMcp-Session-Id: opaque-session\r\nContent-Length: %lu\r\n\r\n%s", (unsigned long)strlen(reply), reply);
        return;
    }
    assert(strstr(c->request, "Mcp-Session-Id: opaque-session\r\n"));
    assert(strstr(c->request, "MCP-Protocol-Version: 2025-11-25\r\n"));
    if (strstr(body, "notifications/initialized") || strstr(body, "notifications/cancelled")) {
        if (strstr(body, "notifications/cancelled")) cancels++;
        strcpy(c->response, "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\n\r\n");
        return;
    }
    if (strstr(body, "tools/list")) {
        lists++;
        if (lists == 1) {
            assert(!strstr(body, "cursor"));
            strcpy(reply, "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{\"name\":\"tavily_search\",\"description\":\"Search the web\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"],\"additionalProperties\":false}}],\"nextCursor\":\"next-page\"}}");
        } else {
            assert(strstr(body, "\"cursor\":\"next-page\""));
            strcpy(reply, "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"tools\":[{\"name\":\"tavily_extract\",\"description\":\"Extract one page\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"],\"additionalProperties\":false}}]}}");
        }
    } else {
        long id;
        calls++;
        scanned = sscanf(body, "{\"jsonrpc\":\"2.0\",\"id\":%ld", &id);
        assert(scanned == 1);
        /* The model's mapped name is dispatched under its original server name
         * and the arguments are forwarded verbatim, never rewritten. */
        if (strstr(body, "\"name\":\"tavily_search\"")) {
            assert(strstr(body, "\"arguments\":{\"query\":\"mac os 9\"}"));
            search_text(text, sizeof(text));
            snprintf(reply, sizeof(reply), "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],\"isError\":false}}", id, text);
        } else {
            assert(strstr(body, "\"name\":\"tavily_extract\"") && strstr(body, "\"arguments\":{\"url\":\"https://example.com/mac-os-9\"}"));
            snprintf(reply, sizeof(reply), "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"Fixture extraction.\"}],\"isError\":false}}", id);
        }
    }
    snprintf(c->response, sizeof(c->response), "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\ndata: %s\n\n", reply);
}
int MacTLS_Read(MacTLS_Context *c, void *bytes, size_t len)
{
    size_t n;
    int reads = 0;
    assert(c->used && len <= 2048); reads++;
    (void)reads;
    if (!c->response[0]) respond(c);
    n = strlen(c->response) - c->received;
    if (n > 64) n = 64; /* many small reads exercise the resumable stream */
    if (n > len) n = len;
    memcpy(bytes, c->response + c->received, n); c->received += n;
    return (int)n;
}
MacTLS_State MacTLS_GetState(const MacTLS_Context *c) { return c->state; }
void MacTLS_Close(MacTLS_Context *c) { assert(c->used && c->state != kMacTLS_Connecting); c->used = 0; closes++; }

/* ---- Client driver ---- */
static unsigned long ticks = 1;
static int step(void)
{
    int r, guard = 0;
    do {
        r = mcp_client_step(&client, ++ticks);
        assert(++guard < 100000);
    } while (r == 0);
    return r;
}
static void parse_config(void)
{
    const char *s = "{\"mcpServers\":{\"tavily\":{\"url\":\"https://mcp.tavily.com/mcp/\",\"headers\":{\"Authorization\":\"Bearer client-secret-key\"}}}}";
    memset(&config, 0, sizeof(config));
    assert(!mcp_config_parse(s, strlen(s), &config, error, sizeof(error)));
}
static void reset(void)
{
    memset(&client, 0, sizeof(client));
    memset(contexts, 0, sizeof(contexts));
    creates = closes = lists = calls = cancels = 0;
    bad_version = reflect = 0;
    assert(agent_set_mcp_schemas(NULL) == 0);
    mcp_result_new_session();
    mcp_run_reset();
    parse_config();
}
/* Discover to READY, mirroring the run loop's discovery state. */
static void discover_ready(void)
{
    int i;
    assert(mcp_client_discover(&client, &config, ++ticks) == 0);
    for (i = 0; i < 1600 && !mcp_client_step(&client, ++ticks); i++) {}
    assert(client.state == MCP_READY && client.registry.count == 2);
    assert(lists == 2 && !strcmp(client.session, "opaque-session"));
}

/* ---- Model round helpers ---- */
static Agent agent;
static char req[CHAT_REQUEST_CAP], response[4096], result[AGENT_RESULT_CAP];
static JsonToken wire[65536];
static char decoded[16384];
static int journal_calls;
static char last_event[64];
static int journal(void *context, const char *event, const char *json)
{
    JsonToken tokens[512];
    (void)context;
    assert(json_parse(json, strlen(json), tokens, 512) > 0);
    journal_calls++;
    snprintf(last_event, sizeof(last_event), "%s", event);
    return 0;
}
/* The tools array of the latest request carries every native row plus each
 * wanted remote row exactly once. */
static void request_pins(const char *const *wanted, int count)
{
    int tools, at, i;
    int hits[8] = {0}, total = 0;
    size_t natives = tools_count();
    assert(count <= 8);
    assert(json_parse(req, strlen(req), wire, 65536) > 0 && wire[0].type == JSON_OBJECT);
    tools = json_member(req, wire, 0, "tools");
    assert(tools > 0 && wire[tools].type == JSON_ARRAY);
    for (at = tools + 1; at < wire[tools].next; at = wire[at].next) {
        char name[96];
        int f = json_member(req, wire, at, "function");
        assert(wire[at].type == JSON_OBJECT && f > 0);
        assert(json_string(req, wire, json_member(req, wire, f, "name"), name, sizeof(name)) > 0);
        total++;
        for (i = 0; i < count; i++) if (!strcmp(name, wanted[i])) {
            assert(!hits[i]);
            hits[i] = 1;
        }
    }
    assert(total == (int)natives + count);
    for (i = 0; i < count; i++) assert(hits[i]);
}
static void model_calls(const char *name, const char *arguments)
{
    char quoted[2048];
    int n;
    assert(json_quote(arguments, quoted, sizeof(quoted)) > 0);
    n = snprintf(response, sizeof(response), "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"%s\",\"arguments\":%s}}]}}]}", name, quoted);
    assert(n > 0 && (size_t)n < sizeof(response));
}
static void begin_run(const char *prompt)
{
    assert(agent_begin(&agent, prompt, error, sizeof(error)) == 0);
    assert(agent_request(&agent, "model", req, sizeof(req)) > 0);
}
/* Retain the completed, redacted client response and hand the model the
 * retention summary, as the run loop's retention state does. */
static void retain_response(void)
{
    McpResultRetain in;
    const char *body = mcp_client_response(&client);
    int r, guard = 0;
    memset(&in, 0, sizeof(in));
    in.response = body;
    in.response_len = strlen(body);
    in.folder = "Retro68:Sherclawk MCP Results:";
    in.config = &client.config;
    in.session = client.session;
    in.journal = journal;
    in.journal_context = NULL;
    assert(*body);
    r = mcp_result_retain_begin(&in, result, sizeof(result));
    while (r == 2) {
        assert(++guard < 100000);
        r = mcp_result_retain_step(result, sizeof(result), 0);
    }
    assert(r == 0);
    assert(!strcmp(last_event, "mcp_result_retained"));
    assert(!agent_tool_result(&agent, result, error, sizeof(error)));
}
/* Field helper over the JSON envelopes in `result`. */
static const char *result_str(const char *key)
{
    JsonToken t[64];
    assert(json_parse(result, strlen(result), t, 64) > 1);
    assert(json_string(result, t, json_member(result, t, 0, key), decoded, sizeof(decoded)) >= 0);
    return decoded;
}
static long result_long(const char *key)
{
    JsonToken t[64];
    long value = -1;
    assert(json_parse(result, strlen(result), t, 64) > 1);
    assert(!json_integer(result, t, json_member(result, t, 0, key), &value));
    return value;
}
/* Page a retained result back through read_mcp_result until whole. */
static void read_back(const char *id, char *out, size_t cap)
{
    static char page[AGENT_RESULT_CAP];
    long start = 0;
    size_t at = 0;
    for (;;) {
        char arguments[96];
        AgentCall call;
        const char *text;
        int truncated;
        JsonToken t[64];
        int text_at;
        snprintf(arguments, sizeof(arguments), "{\"result_id\":\"%s\",\"start_byte\":%ld}", id, start);
        memset(&call, 0, sizeof(call));
        strcpy(call.name, "read_mcp_result");
        strcpy(call.arguments, arguments);
        assert(mcp_result_read(&call, page, sizeof(page)) == 0);
        assert(strstr(page, "\"status\":\"ok\""));
        assert(json_parse(page, strlen(page), t, 64) > 1);
        text_at = json_member(page, t, 0, "text");
        assert(text_at > 0);
        assert(json_string(page, t, text_at, decoded, sizeof(decoded)) >= 0);
        text = decoded;
        assert(strlen(text) < cap - at);
        memcpy(out + at, text, strlen(text) + 1);
        at += strlen(text);
        truncated = strstr(page, "\"truncated\":true") != NULL;
        if (!truncated) break;
        {
            long value = -1;
            assert(!json_integer(page, t, json_member(page, t, 0, "next_byte"), &value));
            assert(value > start);
            start = value;
        }
    }
}

int main(void)
{
    const char *names[2];
    char id[32], text[4096];
    int n;

    /* ---- Run start: parse, discover, freeze. ---- */
    reset();
    reflect = 1;
    begin_run("Search the web for Mac OS 9 notes.");
    discover_ready();
    assert(agent_set_mcp_schemas(client.registry.schemas) == 0);
    assert(agent_mcp_tool_count() == 2);
    assert(!strcmp(agent_mcp_tool_name(0), "mcp_tavily_tavily_search"));
    assert(!strcmp(agent_mcp_tool_name(1), "mcp_tavily_tavily_extract"));
    names[0] = "mcp_tavily_tavily_search"; names[1] = "mcp_tavily_tavily_extract";
    assert(agent_request(&agent, "model", req, sizeof(req)) > 0);
    request_pins(names, 2);

    /* ---- A model-selected remote call, dispatched sequentially. ---- */
    model_calls("mcp_tavily_tavily_search", "{\"query\":\"mac os 9\"}");
    assert(!agent_response(&agent, response, strlen(response), 200, error, sizeof(error)));
    assert(agent.count == 1 && !strcmp(agent.calls[0].name, "mcp_tavily_tavily_search"));
    assert(!strcmp(agent.calls[0].arguments, "{\"query\":\"mac os 9\"}"));
    assert(mcp_client_call(&client, agent.calls[0].name, agent.calls[0].arguments, ++ticks) == 0);
    /* Only one remote call is in flight at a time. */
    assert(mcp_client_call(&client, agent.calls[0].name, "{}", ++ticks) == -1);
    assert(step() == 1);
    assert(client.state == MCP_COMPLETE && calls == 1);
    /* The completed response is redacted before anything else sees it. */
    assert(!strstr(mcp_client_response(&client), "client-secret-key"));
    assert(!strstr(mcp_client_response(&client), "opaque-session"));

    /* ---- Retention: the summary is the model-facing tool result. ---- */
    n = journal_calls;
    retain_response();
    assert(journal_calls == n + 1);
    assert(!strcmp(result_str("status"), "ok"));
    assert(result_long("bytes") > 512 && result_long("items") == 1);
    snprintf(id, sizeof(id), "%s", result_str("result_id"));
    assert(strlen(id) == 17 && !strncmp(id, "mcp-", 4));
    assert(strstr(result, "https://example.com/mac-os-9"));
    assert(strstr(result, "https://example.com/retro68-notes"));
    assert(!strstr(result, "client-secret-key"));
    /* The artifact is published, verified and staged leftovers are gone. */
    assert(mcp_run_file_count() == 1);
    assert(mcp_run_file_count_suffix(".new") == 0);
    assert(mcp_run_file_bytes(0) == result_long("bytes"));

    /* ---- Paging: the model reads the artifact back through
     * read_mcp_result; multi-byte UTF-8 survives the page boundaries. ---- */
    read_back(id, text, sizeof(text));
    assert(strstr(text, "Paragraph 15 caf\xc3\xa9 repeats"));
    assert(strstr(text, "https://example.com/mac-os-9"));
    assert(!strstr(text, "client-secret-key") && !strstr(text, "opaque-session"));
    assert(!strstr(text, "u00e9"));

    /* ---- A second, sequential call: ordering is one call, one result. ---- */
    model_calls("mcp_tavily_tavily_extract", "{\"url\":\"https://example.com/mac-os-9\"}");
    assert(!agent_response(&agent, response, strlen(response), 200, error, sizeof(error)));
    assert(mcp_client_call(&client, agent.calls[0].name, agent.calls[0].arguments, ++ticks) == 0);
    assert(step() == 1);
    assert(calls == 2);
    retain_response();
    assert(mcp_run_file_count() == 2 && mcp_run_file_count_suffix(".new") == 0);
    /* The next request still carries the frozen remote schemas. */
    assert(agent_request(&agent, "model", req, sizeof(req)) > 0);
    request_pins(names, 2);

    /* ---- Stop during a call: no further calls, drained contexts, and the
     * cancellation is best-effort (one notification, no replay). ---- */
    model_calls("mcp_tavily_tavily_search", "{\"query\":\"stopped\"}");
    assert(!agent_response(&agent, response, strlen(response), 200, error, sizeof(error)));
    assert(mcp_client_call(&client, agent.calls[0].name, agent.calls[0].arguments, ++ticks) == 0);
    mcp_client_stop(&client, ++ticks);
    {
        int r, guard = 0;
        do {
            r = mcp_client_step(&client, ++ticks);
            assert(++guard < 10000);
        } while (r == 0);
        assert(r == -1);
    }
    assert(client.state == MCP_STOPPED && cancels == 1);
    assert(!mcp_client_response(&client)[0]);
    assert(mcp_client_call(&client, "mcp_tavily_tavily_search", "{}", ++ticks) == -1);
    assert(mcp_run_file_count() == 2);
    assert(creates == closes);
    /* Stop clears the frozen advertisement; the next run re-discovers. */
    assert(agent_set_mcp_schemas(NULL) == 0 && agent_mcp_tool_count() == 0);
    assert(!agent_stop(&agent, "stopped"));

    /* ---- Stop during result publication: the stage is deleted, nothing is
     * advertised and the call is not replayed. ---- */
    reset();
    begin_run("Another search.");
    reflect = 1;
    discover_ready();
    assert(agent_set_mcp_schemas(client.registry.schemas) == 0);
    model_calls("mcp_tavily_tavily_search", "{\"query\":\"mac os 9\"}");
    assert(!agent_response(&agent, response, strlen(response), 200, error, sizeof(error)));
    assert(mcp_client_call(&client, agent.calls[0].name, agent.calls[0].arguments, ++ticks) == 0);
    assert(step() == 1);
    {
        McpResultRetain in;
        int r;
        memset(&in, 0, sizeof(in));
        in.response = mcp_client_response(&client);
        in.response_len = strlen(in.response);
        in.folder = "Retro68:Sherclawk MCP Results:";
        in.config = &client.config;
        in.session = client.session;
        in.journal = journal;
        in.journal_context = NULL;
        assert(mcp_result_retain_begin(&in, result, sizeof(result)) == 2);
        r = mcp_result_retain_step(result, sizeof(result), 1);
        assert(r == 1);
        assert(strstr(result, "\"code\":\"STOPPED\""));
        assert(mcp_run_file_count() == 0 && mcp_run_file_count_suffix(".new") == 0);
        assert(!agent_tool_result(&agent, result, error, sizeof(error)));
    }
    assert(mcp_client_close(&client) == 0);
    assert(creates == closes);
    assert(!agent_stop(&agent, "stopped during retention"));

    /* ---- Failed discovery: safe native-only fallback, no freeze. ---- */
    reset();
    bad_version = 1;
    begin_run("Search again.");
    assert(mcp_client_discover(&client, &config, ++ticks) == 0);
    assert(step() == -1);
    assert(client.state == MCP_FAILED && client.registry.count == 0 && !client.registry.schemas[0]);
    assert(strstr(client.error, "unsupported initialization"));
    /* No remote schemas are frozen; the request is native-only. */
    assert(agent_mcp_tool_count() == 0);
    assert(agent_request(&agent, "model", req, sizeof(req)) > 0);
    request_pins(NULL, 0);
    assert(mcp_client_close(&client) == 0);
    assert(!agent_stop(&agent, "discovery failed"));

    /* ---- Missing/disabled configuration causes no MCP traffic. ---- */
    reset();
    {
        McpConfig disabled;
        memset(&disabled, 0, sizeof(disabled));
        assert(!mcp_config_parse("{\"mcpServers\":{}}", 17, &disabled, error, sizeof(error)));
        assert(!disabled.enabled);
        n = creates;
        assert(mcp_client_discover(&client, &disabled, ++ticks) == 1);
        assert(client.state == MCP_READY && creates == n && client.registry.count == 0);
    }
    begin_run("Native only.");
    assert(agent_request(&agent, "model", req, sizeof(req)) > 0);
    request_pins(NULL, 0);

    puts("PASS MCP run integration: discovery, freeze, sequential dispatch, retention, paging, Stop, fallback");
    return 0;
}
