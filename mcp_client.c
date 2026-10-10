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
    c->exchange.stream.message_len = 0; c->exchange.stream.message[0] = 0;
    return 0;
}
static int fail(McpClient *c, const char *message)
{
    snprintf(c->error, sizeof(c->error), "MCP unavailable: %s.", message);
    int pending;
    c->registry.count = 0; c->registry.schema_len = 0; c->registry.schemas[0] = 0;
    c->working = 0; c->pending_len = 0;
    pending = mcp_client_close(c);
    c->state = pending ? MCP_FAIL_DRAIN : MCP_FAILED;
    return pending ? 0 : -1;
}
static int start(McpClient *c, McpExchange *e, const char *body, int ack)
{
    int n;
    if (e->ctx) return -1;
    n = mcp_post(&c->config, c->session, c->version[0] ? c->version : NULL,
                 body, e->request, sizeof(e->request));
    if (n < 0) return -1;
    e->length = (size_t)n; e->sent = 0;
    mcp_stream_init(&e->stream, ack);
    if (e == &c->exchange) c->pending_len = 0;
    e->ctx = MacTLS_Create(c->config.host, c->config.port);
    if (!e->ctx) { memset(e->request, 0, sizeof(e->request)); return -1; }
    return 0;
}
/* -1 failure, 0 nothing yet, 1 exchange complete, 3 a message is ready in the
 * stream and is processed by later steps. */
static int step_exchange(McpClient *c, McpExchange *e)
{
    MacTLS_State state;
    int i, drained = 0;
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
        size_t len;
        int r;
        if (e == &c->exchange && c->pending_len) {
            len = c->pending_len;
            memcpy(bytes, c->pending, len); c->pending_len = 0;
        } else {
            int read = MacTLS_Read(e->ctx, bytes, sizeof(bytes));
            if (read < 0) return -1;
            if (!read) { drained = 1; break; }
            if ((size_t)read > sizeof(bytes)) return -1;
            len = (size_t)read;
        }
        r = mcp_stream_feed(&e->stream, bytes, len);
        if (r < 0) return -1;
        if (r == 2) {
            /* Bytes after the message wait for it to be processed. */
            c->pending_len = len - e->stream.used;
            memcpy(c->pending, bytes + e->stream.used, c->pending_len);
            return 3;
        }
        if (r > 0) return e->sent == e->length ? 1 : -1;
    }
    /* A closed connection may still hold buffered plaintext; only an empty
     * read means the stream truly ended. */
    if (state == kMacTLS_Closed && drained) {
        int r;
        if (e->sent != e->length) return -1;
        r = mcp_stream_eof(&e->stream);
        return r == 2 ? 3 : r;
    }
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
    c->id++; c->started = ticks; c->call_error = 0; c->working = 0; c->pending_len = 0;
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
    /* Abandon any message being processed, and a discovery it half built. */
    if (c->state == MCP_INITIALIZE || c->state == MCP_INITIALIZED || c->state == MCP_LIST) {
        c->registry.count = 0; c->registry.schema_len = 0; c->registry.schemas[0] = 0;
    }
    c->working = 0; c->pending_len = 0;
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
        r = step_exchange(c, &c->auxiliary);
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
        r = step_exchange(c, &c->auxiliary);
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
    /* A received message is processed in budgeted steps, which a Stop between
     * steps abandons. */
    if (c->working) {
        if (!mcp_work_step(&c->work, MCP_WORK_BUDGET)) return 0;
        c->working = 0;
        if (c->work.rpc < 0) return fail(c, "connection, HTTP framing or RPC failure (no retry)");
        if (c->work.rpc == 2) {
            if (c->reply_count == 4) return fail(c, "connection, HTTP framing or RPC failure (no retry)");
            strcpy(c->replies[c->reply_count++], c->reply);
        }
        mcp_stream_resume(&c->exchange.stream, c->work.rpc == 1);
        if (c->exchange.stream.failed || (c->work.rpc == 1 && c->exchange.sent != c->exchange.length))
            return fail(c, "connection, HTTP framing or RPC failure (no retry)");
        if (c->work.rpc != 1) return 0;
        r = 1;
    } else {
        /* Matching response can arrive in the same read as a server request;
         * honor pending replies before transitioning to the next RPC. */
        r = c->exchange.stream.done ? 1 : step_exchange(c, &c->exchange);
        if (r == 3) {
            const char *session = c->exchange.stream.session;
            int stages = MCP_STAGE_CLASSIFY | (c->state == MCP_INITIALIZE ? MCP_STAGE_INITIALIZE :
                c->state == MCP_LIST ? MCP_STAGE_DISCOVER : c->state == MCP_CALL ? MCP_STAGE_CALL : 0);
            if (session[0]) {
                if (c->state == MCP_INITIALIZE) strcpy(c->session, session);
                else if (strcmp(c->session, session)) return fail(c, "session changed unexpectedly");
            }
            mcp_work_init(&c->work, stages, &c->config, &c->registry, c->exchange.stream.message,
                c->exchange.stream.message_len, c->id, c->reply, sizeof(c->reply), c->version, sizeof(c->version));
            c->work.seed = (uint32_t)ticks * 2654435761u + (uint32_t)c->exchange.stream.message_len;
            c->working = 1;
            return 0;
        }
    }
    if (r < 0) return fail(c, "connection, HTTP framing or RPC failure (no retry)");
    if (c->reply_count || r == 0) return 0;
    close_exchange(&c->exchange);
    if (c->state == MCP_INITIALIZE) {
        if (c->work.status < 0) return fail(c, "unsupported initialization/version");
        c->state = MCP_INITIALIZED;
        return start(c, &c->exchange, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", 1) < 0 ? fail(c, "initialized notification failed") : 0;
    }
    if (c->state == MCP_INITIALIZED) return list(c);
    if (c->state == MCP_LIST) {
        if (c->work.status < 0) return fail(c, "invalid or over-budget discovery");
        if (c->work.more) return list(c);
        c->state = MCP_READY; return 1;
    }
    if (c->state == MCP_CALL) {
        c->call_error = c->work.call_error;
        c->state = MCP_COMPLETE; return 1;
    }
    return fail(c, "invalid client state");
}
const char *mcp_client_response(const McpClient *c)
{
    return c->state == MCP_COMPLETE ? c->exchange.stream.message : "";
}
