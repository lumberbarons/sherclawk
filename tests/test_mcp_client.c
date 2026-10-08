/* Fake TLS exercises actual cooperative client state transitions; no API key.
 * Request bodies select independently authored response fixtures. */
#include "mcp_client.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct MacTLS_Context {
    char request[12000], response[12000];
    size_t sent, received;
    int used, pumps;
    MacTLS_State state;
};
static struct MacTLS_Context contexts[2];
static McpClient client;
static McpConfig config;
static int creates, closes, lists, calls, cancels, ping_replies, reads_in_step;
static int connecting, stalled, bad_version, bad_status, tool_error, disconnect, with_ping;
static char error[256];
MacTLS_Context *MacTLS_Create(const char *host, uint16_t port)
{
    int i;
    assert(!strcmp(host, "mcp.tavily.com") && port == 443);
    for (i = 0; i < 2; i++) if (!contexts[i].used) {
        memset(&contexts[i], 0, sizeof(contexts[i])); contexts[i].used = 1;
        contexts[i].state = connecting ? kMacTLS_Connecting : kMacTLS_Connected;
        creates++; return &contexts[i];
    }
    assert(0); return NULL;
}
MacTLS_State MacTLS_Pump(MacTLS_Context *c)
{
    assert(c->used); c->pumps++;
    if (connecting) return c->state = kMacTLS_Connecting;
    if (stalled) return c->state = kMacTLS_Handshaking;
    if (disconnect && c->sent) return kMacTLS_Closed;
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
static void respond(MacTLS_Context *c)
{
    char *body = strstr(c->request, "\r\n\r\n"), *length = strstr(c->request, "Content-Length: ");
    long wanted;
    int scanned;
    char reply[8000];
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
    if (strstr(body, "notifications/initialized") || strstr(body, "notifications/cancelled") || strstr(body, "\"id\":\"ping-fixture\"")) {
        if (strstr(body, "notifications/cancelled")) cancels++;
        if (strstr(body, "\"id\":\"ping-fixture\"")) { assert(strstr(body, "\"result\":{}")); ping_replies++; }
        strcpy(c->response, "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\n\r\n"); return;
    }
    if (strstr(body, "tools/list")) {
        lists++;
        if (lists == 1) {
            assert(!strstr(body, "cursor"));
            strcpy(reply, "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{\"name\":\"tavily_search\",\"inputSchema\":{\"type\":\"object\"}}],\"nextCursor\":\"next-page\"}}");
        } else {
            assert(strstr(body, "\"cursor\":\"next-page\""));
            strcpy(reply, "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"tools\":[{\"name\":\"tavily_extract\",\"inputSchema\":{\"type\":\"object\"}}]}}");
        }
    } else {
        long id;
        calls++;
        scanned = sscanf(body, "{\"jsonrpc\":\"2.0\",\"id\":%ld", &id);
        assert(scanned == 1);
        assert(strstr(body, "\"name\":\"tavily_search\""));
        assert(strstr(body, "\"arguments\":{\"query\":\"fixture\"}"));
        snprintf(reply, sizeof(reply), "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"fixture\"}],\"isError\":%s}}", id, tool_error ? "true" : "false");
    }
    snprintf(c->response, sizeof(c->response), "HTTP/1.1 %s\r\nContent-Type: text/event-stream\r\n\r\n%sdata: %s\n\n", bad_status ? "500 Error" : "200 OK", with_ping && lists == 1 ? "data: {\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":\"ping-fixture\"}\n\n" : "", reply);
}
int MacTLS_Read(MacTLS_Context *c, void *bytes, size_t len)
{
    size_t n;
    assert(c->used && len <= 2048); reads_in_step++;
    assert(reads_in_step <= 4);
    if (!c->response[0]) respond(c);
    n = strlen(c->response) - c->received;
    if (n > 3) n = 3;
    if (n > len) n = len;
    memcpy(bytes, c->response + c->received, n); c->received += n;
    return (int)n;
}
MacTLS_State MacTLS_GetState(const MacTLS_Context *c) { return c->state; }
void MacTLS_Close(MacTLS_Context *c) { assert(c->used && c->state != kMacTLS_Connecting); c->used = 0; closes++; }
static int step(unsigned long ticks)
{
    reads_in_step = 0; return mcp_client_step(&client, ticks);
}
static void expect_step(unsigned long ticks, int expected)
{
    int result = step(ticks);
    assert(result == expected);
}
static void reset(void)
{
    memset(&client, 0, sizeof(client)); memset(contexts, 0, sizeof(contexts));
    creates = closes = lists = calls = cancels = ping_replies = 0;
    connecting = stalled = bad_version = bad_status = tool_error = disconnect = with_ping = 0;
    {
        const char *s = "{\"mcpServers\":{\"tavily\":{\"url\":\"https://mcp.tavily.com/mcp/\"}}}";
        assert(!mcp_config_parse(s, strlen(s), &config, error, sizeof(error)));
    }
}
static void ready(void)
{
    int i;
    assert(mcp_client_discover(&client, &config, 0) == 0);
    for (i = 0; i < 1600 && !step((unsigned long)i); i++) {}
    assert(client.state == MCP_READY && client.registry.count == 2);
    assert(lists == 2 && !strcmp(client.session, "opaque-session"));
}
int main(void)
{
    int i, n;
    reset(); with_ping = 1; ready(); assert(ping_replies == 1);
    assert(!mcp_client_call(&client, "mcp_tavily_tavily_search", "{\"query\":\"fixture\"}", 2000));
    for (i = 2000; i < 3000 && !step((unsigned long)i); i++) {}
    assert(client.state == MCP_COMPLETE && !client.call_error && calls == 1);
    tool_error = 1;
    assert(!mcp_client_call(&client, "mcp_tavily_tavily_search", "{\"query\":\"fixture\"}", 3000));
    for (i = 3000; i < 4000 && !step((unsigned long)i); i++) {}
    assert(client.state == MCP_COMPLETE && client.call_error && calls == 2);
    mcp_client_close(&client); assert(creates == closes && !client.session[0]);
    reset(); stalled = 1; assert(!mcp_client_discover(&client, &config, 0));
    expect_step(1799, 0); expect_step(1800, -1);
    assert(client.state == MCP_FAILED && !lists);
    n = creates; assert(mcp_client_call(&client, "mcp_tavily_tavily_search", "{}", 1801) < 0 && creates == n);
    reset(); bad_version = 1; assert(!mcp_client_discover(&client, &config, 0));
    for (i = 0; i < 1700 && !step((unsigned long)i); i++) {}
    assert(client.state == MCP_FAILED && !lists && creates == 1);
    reset(); ready();
    assert(!mcp_client_call(&client, "mcp_tavily_tavily_search", "{\"query\":\"fixture\"}", 2000));
    mcp_client_stop(&client, 2001);
    for (i = 2001; i < 2200 && !step((unsigned long)i); i++) {}
    assert(client.state == MCP_STOPPED && cancels == 1 && calls == 0 && creates == closes);
    reset(); stalled = 1; assert(!mcp_client_discover(&client, &config, 0));
    mcp_client_stop(&client, 1);
    expect_step(2, -1);
    assert(client.state == MCP_STOPPED && !cancels && creates == closes);
    reset(); ready(); disconnect = 1;
    assert(!mcp_client_call(&client, "mcp_tavily_tavily_search", "{\"query\":\"fixture\"}", 2000));
    expect_step(2001, 0);
    expect_step(2002, -1);
    assert(client.state == MCP_FAILED && !calls);
    reset(); ready(); stalled = 1;
    assert(!mcp_client_call(&client, "mcp_tavily_tavily_search", "{\"query\":\"fixture\"}", 2000));
    expect_step(9199, 0); expect_step(9200, -1);
    assert(client.state == MCP_FAILED);
    reset(); connecting = 1;
    assert(!mcp_client_discover(&client, &config, 0));
    mcp_client_stop(&client, 1); expect_step(2, 0);
    assert(!closes && client.state == MCP_STOPPING);
    connecting = 0; expect_step(3, -1);
    assert(creates == closes && client.state == MCP_STOPPED && !calls);
    reset(); connecting = 1;
    assert(!mcp_client_discover(&client, &config, 0));
    expect_step(1800, 0); assert(client.state == MCP_FAIL_DRAIN && !closes);
    connecting = 0; expect_step(1801, -1); assert(creates == closes);
    puts("PASS MCP cooperative discovery, pagination, calls, ping, deadlines, failures and Stop");
    return 0;
}
