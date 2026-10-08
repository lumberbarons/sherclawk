#include "mcp_client.h"
#include <stdio.h>
#include <string.h>
static int close_exchange(McpExchange *e)
{
    if (e->ctx) {
        MacTLS_State state = MacTLS_GetState(e->ctx);
        if (state == kMacTLS_Connecting || state == kMacTLS_Idle) return 1;
    }
    if (e->ctx) MacTLS_Close(e->ctx);
    e->ctx = NULL;
    memset(e->request, 0, sizeof(e->request));
    return 0;
}
int mcp_client_close(McpClient *c)
{
    int pending = close_exchange(&c->exchange);
    pending |= close_exchange(&c->auxiliary);
    if (pending) {
        c->state = MCP_STOPPING; c->stop_phase = 0; c->cancel_pending = 0;
        return 1;
    }
    memset(c->session, 0, sizeof(c->session));
    memset(&c->config, 0, sizeof(c->config));
    memset(c->replies, 0, sizeof(c->replies)); c->reply_count = 0;
    return 0;
}
static int fail(McpClient *c, const char *message)
{
    snprintf(c->error, sizeof(c->error), "MCP unavailable: %s.", message);
    int pending;
    c->registry.count = 0; c->registry.schema_len = 0; c->registry.schemas[0] = 0;
    pending = mcp_client_close(c);
    c->state = pending ? MCP_FAIL_DRAIN : MCP_FAILED;
    return pending ? 0 : -1;
}
static int receive(void *context, const char *s)
{
    McpClient *c = context;
    char reply[2048];
    int r;
    if (c->state == MCP_INITIALIZE && c->exchange.stream.session[0])
        strcpy(c->session, c->exchange.stream.session);
    r = mcp_rpc(s, c->id, reply, sizeof(reply));
    if (r == 1) {
        size_t len = strlen(s);
        if (len > MCP_MESSAGE_CAP) return -1;
        memcpy(c->response, s, len + 1);
        return 1;
    }
    if (r == 2) {
        if (c->reply_count == 4) return -1;
        strcpy(c->replies[c->reply_count++], reply);
        return 0;
    }
    return r;
}
static int start(McpClient *c, McpExchange *e, const char *body, int ack)
{
    int n;
    if (e->ctx) return -1;
    n = mcp_post(&c->config, c->session, c->version[0] ? c->version : NULL,
                 body, e->request, sizeof(e->request));
    if (n < 0) return -1;
    e->length = (size_t)n; e->sent = 0;
    mcp_stream_init(&e->stream, ack, ack ? NULL : receive, c);
    e->ctx = MacTLS_Create(c->config.host, c->config.port);
    if (!e->ctx) { memset(e->request, 0, sizeof(e->request)); return -1; }
    return 0;
}
static int step_exchange(McpExchange *e)
{
    MacTLS_State state;
    int i;
    char bytes[2048];
    if (!e->ctx) return -1;
    state = MacTLS_Pump(e->ctx);
    if (state == kMacTLS_Error) return -1;
    if (state == kMacTLS_Connected && e->sent < e->length) {
        size_t amount = e->length - e->sent;
        int wrote;
        if (amount > 2048) amount = 2048;
        wrote = MacTLS_Write(e->ctx, e->request + e->sent, amount);
        if (wrote < 0 || (size_t)wrote > amount) return -1;
        e->sent += (size_t)wrote;
    }
    if (state != kMacTLS_Connected && state != kMacTLS_Closed) return 0;
    for (i = 0; i < 4; i++) {
        int read = MacTLS_Read(e->ctx, bytes, sizeof(bytes)), r;
        if (read < 0) return -1;
        if (!read) break;
        if ((size_t)read > sizeof(bytes)) return -1;
        r = mcp_stream_feed(&e->stream, bytes, (size_t)read);
        if (r < 0) return -1;
        if (r > 0) return e->sent == e->length ? 1 : -1;
    }
    if (state == kMacTLS_Closed) return e->sent == e->length ? mcp_stream_eof(&e->stream) : -1;
    return 0;
}
int mcp_client_discover(McpClient *c, const McpConfig *config, unsigned long ticks)
{
    static const char initialize[] = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"" MCP_VERSION "\",\"capabilities\":{},\"clientInfo\":{\"name\":\"Sherclawk\",\"version\":\"1.0\"}}}";
    if (c->exchange.ctx || c->auxiliary.ctx) return -1;
    memset(c, 0, sizeof(*c));
    c->config = *config; c->started = ticks; c->id = 1;
    if (!config->enabled) { c->state = MCP_READY; return 1; }
    c->state = MCP_INITIALIZE;
    return start(c, &c->exchange, initialize, 0) < 0 ? fail(c, "TLS initialization failed") : 0;
}
static int list(McpClient *c)
{
    char quoted[2048], body[2304];
    c->id++;
    if (c->registry.cursor[0]) {
        if (json_quote(c->registry.cursor, quoted, sizeof(quoted)) < 0) return fail(c, "cursor limit");
        snprintf(body, sizeof(body), "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"method\":\"tools/list\",\"params\":{\"cursor\":%s}}", c->id, quoted);
    } else snprintf(body, sizeof(body), "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"method\":\"tools/list\",\"params\":{}}", c->id);
    c->state = MCP_LIST;
    return start(c, &c->exchange, body, 0) < 0 ? fail(c, "discovery request failed") : 0;
}
int mcp_client_call(McpClient *c, const char *name, const char *arguments, unsigned long ticks)
{
    static char body[MCP_MESSAGE_CAP + 1];
    static JsonToken t[2048];
    int i, n;
    if ((c->state != MCP_READY && c->state != MCP_COMPLETE) || c->exchange.ctx || c->auxiliary.ctx) return -1;
    for (i = 0; i < c->registry.count; i++) if (!strcmp(c->registry.tools[i].name, name)) break;
    if (i == c->registry.count || strlen(arguments) > 8192 || json_parse(arguments, strlen(arguments), t, 2048) < 1 ||
        t[0].type != JSON_OBJECT) return -1;
    c->id++; c->started = ticks; c->call_error = 0; c->response[0] = 0;
    n = snprintf(body, sizeof(body), "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"method\":\"tools/call\",\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
        c->id, c->registry.tools[i].original, arguments);
    if (n < 0 || (size_t)n >= sizeof(body)) return -1;
    c->state = MCP_CALL;
    return start(c, &c->exchange, body, 0) < 0 ? fail(c, "tool request failed") : 0;
}
void mcp_client_stop(McpClient *c, unsigned long ticks)
{
    if (c->state == MCP_STOPPING || c->state == MCP_STOPPED) return;
    c->cancel_pending = c->state == MCP_LIST || c->state == MCP_CALL;
    c->reply_count = 0; c->stop_started = ticks;
    c->stop_phase = 0; c->state = MCP_STOPPING;
    /* Never free a context with an asynchronous OT connect outstanding. */
    close_exchange(&c->exchange); close_exchange(&c->auxiliary);
}
static int drain(McpExchange *e)
{
    if (!e->ctx) return 1;
    MacTLS_Pump(e->ctx);
    return !close_exchange(e);
}
int mcp_client_step(McpClient *c, unsigned long ticks)
{
    int r;
    unsigned long deadline = c->state == MCP_CALL ? 120UL * 60UL : 30UL * 60UL;
    if (c->state == MCP_READY || c->state == MCP_COMPLETE) return 1;
    if (c->state == MCP_FAILED || c->state == MCP_STOPPED || c->state == MCP_IDLE) return -1;
    if (c->state == MCP_STOPPING || c->state == MCP_FAIL_DRAIN) {
        if (!c->stop_phase || c->state == MCP_FAIL_DRAIN) {
            int a = drain(&c->exchange), b = drain(&c->auxiliary);
            if (!a || !b) return 0; /* Certainly owns its connect timeout. */
            if (c->state == MCP_FAIL_DRAIN) {
                mcp_client_close(c); c->state = MCP_FAILED; return -1;
            }
            if (c->cancel_pending) {
                char body[256];
                c->cancel_pending = 0; c->stop_phase = 1; c->stop_started = ticks;
                snprintf(body, sizeof(body), "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":%ld,\"reason\":\"User stopped the run\"}}", c->id);
                if (!start(c, &c->auxiliary, body, 1)) return 0;
            }
            mcp_client_close(c); c->state = MCP_STOPPED; return -1;
        }
        r = step_exchange(&c->auxiliary);
        if (r || (unsigned long)(ticks - c->stop_started) >= 5UL * 60UL) {
            if (mcp_client_close(c)) return 0;
            c->state = MCP_STOPPED; return -1;
        }
        return 0;
    }
    if ((unsigned long)(ticks - c->started) >= deadline) return fail(c, "deadline exceeded (no retry)");
    /* Server requests are acknowledged on a separate POST, while keeping the
     * originating response stream alive. Only one exchange is read per step. */
    if (c->auxiliary.ctx) {
        r = step_exchange(&c->auxiliary);
        if (r < 0) return fail(c, "server-request acknowledgment failed");
        if (r > 0) close_exchange(&c->auxiliary);
        return 0;
    }
    if (c->reply_count) {
        if (start(c, &c->auxiliary, c->replies[0], 1) < 0) return fail(c, "server-request reply failed");
        c->reply_count--;
        memmove(c->replies[0], c->replies[1], (size_t)c->reply_count * sizeof(c->replies[0]));
        memset(c->replies[c->reply_count], 0, sizeof(c->replies[0]));
        return 0;
    }
    /* Matching response can arrive in the same read as a server request;
     * honor pending replies before transitioning to the next RPC. */
    r = c->exchange.stream.done ? 1 : step_exchange(&c->exchange);
    if (r < 0) return fail(c, "connection, HTTP framing or RPC failure (no retry)");
    if (c->reply_count || r == 0) return 0;
    if (c->exchange.stream.session[0]) {
        if (c->state == MCP_INITIALIZE) strcpy(c->session, c->exchange.stream.session);
        else if (strcmp(c->session, c->exchange.stream.session)) return fail(c, "session changed unexpectedly");
    }
    close_exchange(&c->exchange);
    if (c->state == MCP_INITIALIZE) {
        if (mcp_initialize_result(c->response, c->version, sizeof(c->version))) return fail(c, "unsupported initialization/version");
        c->state = MCP_INITIALIZED;
        return start(c, &c->exchange, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", 1) < 0 ? fail(c, "initialized notification failed") : 0;
    }
    if (c->state == MCP_INITIALIZED) return list(c);
    if (c->state == MCP_LIST) {
        r = mcp_discover_page(&c->config, &c->registry, c->response);
        if (r < 0) return fail(c, "invalid or over-budget discovery");
        if (!r) return list(c);
        c->state = MCP_READY; c->response[0] = 0; return 1;
    }
    if (c->state == MCP_CALL) {
        static JsonToken t[8192];
        int result, hint;
        if (json_parse(c->response, strlen(c->response), t, 8192) < 1) return fail(c, "invalid tool response");
        c->call_error = json_member(c->response, t, 0, "error") >= 0;
        result = json_member(c->response, t, 0, "result");
        hint = result >= 0 && t[result].type == JSON_OBJECT ? json_member(c->response, t, result, "isError") : -1;
        if (hint >= 0 && t[hint].end - t[hint].start == 4 && !memcmp(c->response + t[hint].start, "true", 4)) c->call_error = 1;
        c->state = MCP_COMPLETE; return 1;
    }
    return fail(c, "invalid client state");
}
