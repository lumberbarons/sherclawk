#include "mcp.h"
#include "text.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static McpConfig config;
static McpRegistry registry;
static McpStream stream;
static char error[256], request[MCP_MESSAGE_CAP + MCP_HEADER_CAP + 1];
static const char good[] = "{\"mcpServers\":{\"tavily\":{\"url\":\"https://mcp.tavily.com/mcp/\",\"headers\":{\"Authorization\":\"Bearer private-key\"},\"tools\":[\"tavily_search\",\"tavily_extract\"]}}}";
static const char response[] = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"text\":\"caf\xc3\xa9\"}}";
static int calls;
static int message(void *context, const char *s)
{
    char reply[1024];
    int r = mcp_rpc(s, *(long *)context, reply, sizeof(reply));
    calls++;
    return r == 1 ? 1 : r == 0 ? 0 : -1;
}
static void bad(const char *s)
{
    assert(mcp_config_parse(s, strlen(s), &config, error, sizeof(error)) == -1);
    assert(!config.enabled && !config.headers[0]);
    assert(!strstr(error, "private-key"));
}
static void configuration(void)
{
    char text[128], editor[128], roundtrip[128];
    assert(!mcp_config_parse("{\"mcpServers\":{}}", 17, &config, error, sizeof(error)));
    assert(!config.enabled);
    bad("{}");
    bad("{\"other\":0,\"mcpServers\":{}}");
    bad("{\"mcpServers\":{},\"other\":0}");
    bad("{\"mcpServers\":{},\"mcp\\u0053ervers\":{}}");
    bad("{\"mcpServers\":{\"a\":{},\"b\":{}}}");
    bad("{\"mcpServers\":{\"a\":{\"url\":\"https://x/\",\"url\":\"https://x/\"}}}");
    bad("{\"mcpServers\":{\"a\":{\"url\":\"https://x/\",\"command\":\"perl\"}}}");
    bad("{\"mcpServers\":{\"a\":{\"url\":\"http://x/\"}}}");
    bad("{\"mcpServers\":{\"a\":{\"url\":\"https://user@x/\"}}}");
    bad("{\"mcpServers\":{\"a\":{\"url\":\"https://x/#fragment\"}}}");
    bad("{\"mcpServers\":{\"a\":{\"url\":\"https://x/\\r\\nInjected\"}}}");
    bad("{\"mcpServers\":{\"a\":{\"url\":\"https://x:0/\"}}}");
    bad("{\"mcpServers\":{\"a\":{\"url\":\"https://x:443evil/\"}}}");
    bad("{\"mcpServers\":{\"a\":{\"url\":\"https://x:443@evil/\"}}}");
    bad("{\"mcpServers\":{\"a\":{\"url\":\"https://x/\",\"headers\":{\"Host\":\"evil\"}}}}");
    bad("{\"mcpServers\":{\"a\":{\"url\":\"https://x/\",\"headers\":{\"Authorization\":\"private-key\\r\\nX: y\"}}}}");
    bad("{\"mcpServers\":{\"a\":{\"url\":\"https://x/\",\"headers\":{\"X-A\":\"1\",\"x-a\":\"2\"}}}}");
    bad("{\"mcpServers\":{\"a\":{\"url\":\"https://x/\",\"tools\":null}}}");
    bad("{\"mcpServers\":{\"a\":{\"url\":\"https://x/\",\"tools\":[\"foo\",\"foo\"]}}}");
    assert(mcp_config_parse(good, MCP_CONFIG_CAP + 1, &config, error, sizeof(error)) < 0);
    assert(!mcp_config_parse(good, strlen(good), &config, error, sizeof(error)));
    assert(config.enabled && config.official_tavily && config.selected_count == 2);
    assert(!strcmp(config.host, "mcp.tavily.com") && !strcmp(config.path, "/mcp/"));
    assert(strstr(config.headers, "Authorization: Bearer private-key\r\n"));
    strcpy(text, "Bearer private-key; private-key; session-secret");
    mcp_redact(&config, "session-secret", text);
    assert(!strstr(text, "private-key") && !strstr(text, "session-secret"));
    assert(text_to_macroman_strict("caf\xc3\xa9\n\\u2603", editor, sizeof(editor)) > 0);
    assert(text_to_utf8(editor, strlen(editor), roundtrip, sizeof(roundtrip)) > 0);
    assert(!strcmp(roundtrip, "caf\xc3\xa9\n\\u2603"));
    assert(text_to_macroman_strict("\xe2\x98\x83", editor, sizeof(editor)) < 0);
}
static void query_key(void)
{
    static McpConfig queried;
    const char url[] = "{\"mcpServers\":{\"tavily\":{\"url\":\"https://mcp.tavily.com/mcp/?v=1&tavilyApiKey=tvly-querysecret\"}}}";
    char text[128];
    assert(!mcp_config_parse(url, strlen(url), &queried, error, sizeof(error)));
    /* The query does not defeat the official-endpoint match... */
    assert(queried.official_tavily && strstr(queried.path, "?v=1&tavilyApiKey=tvly-querysecret"));
    /* ...and its key is redacted, while a short value is left alone. */
    strcpy(text, "echo tvly-querysecret v=1");
    mcp_redact(&queried, NULL, text);
    assert(!strstr(text, "tvly-querysecret") && strstr(text, "v=1"));
}
static void protocol(void)
{
    char reply[1024], version[32];
    assert(mcp_rpc(response, 1, reply, sizeof(reply)) == 1);
    assert(mcp_rpc(response, 2, reply, sizeof(reply)) < 0);
    assert(mcp_rpc("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\",\"params\":{}}", 1, reply, sizeof(reply)) == 0);
    assert(mcp_rpc("{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":\"x\"}", 1, reply, sizeof(reply)) == 2);
    assert(!strcmp(reply, "{\"jsonrpc\":\"2.0\",\"id\":\"x\",\"result\":{}}"));
    assert(mcp_rpc("{\"jsonrpc\":\"2.0\",\"method\":\"sampling/createMessage\",\"id\":5}", 1, reply, sizeof(reply)) == 2);
    assert(strstr(reply, "-32601"));
    assert(mcp_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{},\"error\":{}}", 1, reply, sizeof(reply)) < 0);
    assert(mcp_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"id\":1,\"result\":{}}", 1, reply, sizeof(reply)) < 0);
    assert(!mcp_initialize_result("{\"result\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"fixture\",\"version\":\"1\"},\"instructions\":\"ignored\"}}", version, sizeof(version)));
    assert(!strcmp(version, "2025-06-18"));
    assert(mcp_initialize_result("{\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{}}}", version, sizeof(version)) < 0);
    assert(mcp_post(&config, "session-secret", MCP_VERSION, "{}", request, sizeof(request)) > 0);
    assert(strstr(request, "Mcp-Session-Id: session-secret\r\n"));
    assert(strstr(request, "MCP-Protocol-Version: " MCP_VERSION "\r\n"));
    assert(strstr(request, "Accept: application/json, text/event-stream\r\n"));
    assert(mcp_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":true,\"message\":\"bad\"}}", 1, reply, sizeof(reply)) < 0);
    assert(mcp_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32601,\"message\":\"bad\"}}", 1, reply, sizeof(reply)) == 1);
    assert(mcp_post(&config, "injected\r\n", MCP_VERSION, "{}", request, sizeof(request)) < 0);
}
static void discover(void)
{
    const char page[] = "{\"result\":{\"tools\":[{\"name\":\"tavily_search\",\"description\":\"Search\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}},{\"name\":\"tavily_crawl\",\"inputSchema\":{}}],\"nextCursor\":\"second\"}}";
    const char last[] = "{\"result\":{\"tools\":[{\"name\":\"tavily_extract\",\"inputSchema\":{\"type\":\"object\"}}]}}";
    const char other[] = "{\"result\":{\"tools\":[{\"name\":\"lookup\",\"annotations\":{\"readOnlyHint\":true},\"inputSchema\":{}}]}}";
    memset(&registry, 0, sizeof(registry));
    assert(mcp_discover_page(&config, &registry, page) == 0);
    assert(registry.count == 1 && !strcmp(registry.cursor, "second"));
    assert(!strcmp(registry.tools[0].name, "mcp_tavily_tavily_search"));
    assert(strstr(registry.schemas, "\"required\":[\"query\"]"));
    assert(mcp_discover_page(&config, &registry, last) == 1 && registry.count == 2);
    assert(mcp_discover_page(&config, &registry, last) < 0 && registry.count == 0);
    memset(&registry, 0, sizeof(registry)); registry.pages = 7;
    assert(mcp_discover_page(&config, &registry, page) < 0 && !registry.count);
    memset(&registry, 0, sizeof(registry)); registry.entries = 64;
    assert(mcp_discover_page(&config, &registry, last) < 0 && !registry.count);
    memset(&registry, 0, sizeof(registry));
    config.official_tavily = 0; config.selection_present = 0;
    assert(mcp_discover_page(&config, &registry, other) == 1 && registry.count == 1);
    memset(&registry, 0, sizeof(registry)); config.selection_present = 1; config.selected_count = 0;
    assert(mcp_discover_page(&config, &registry, other) == 1 && registry.count == 0);
    /* Many keys stay cheap to validate, and an escaped repeat is still found. */
    {
        static char big[MCP_MESSAGE_CAP];
        size_t at = (size_t)snprintf(big, sizeof(big), "{\"result\":{\"tools\":[{\"name\":\"tavily_search\",\"inputSchema\":{\"properties\":{");
        int i;
        config.official_tavily = 1; config.selection_present = 0;
        for (i = 0; i < 1000; i++) at += (size_t)snprintf(big + at, sizeof(big) - at, "%s\"k%d\":{}", i ? "," : "", i);
        memset(&registry, 0, sizeof(registry));
        strcpy(big + at, "}}}]}}");
        assert(mcp_discover_page(&config, &registry, big) == 1 && registry.count == 1);
        memset(&registry, 0, sizeof(registry));
        strcpy(big + at, ",\"k\\u0037\":{}}}}]}}");
        assert(mcp_discover_page(&config, &registry, big) < 0 && !registry.count);
    }
    /* An over-long name skips only that tool; its neighbours survive. */
    {
        char longname[512];
        snprintf(longname, sizeof(longname), "{\"result\":{\"tools\":[{\"name\":\"%0100d\",\"inputSchema\":{}},{\"name\":\"tavily_search\",\"inputSchema\":{\"type\":\"object\"}}]}}", 7);
        memset(&registry, 0, sizeof(registry));
        assert(mcp_discover_page(&config, &registry, longname) == 1);
        assert(registry.count == 1 && registry.entries == 2 && strstr(registry.notice, "unsupported name"));
    }
}
static void fragments(const char *wire, size_t width)
{
    long id = 1;
    size_t at, n = strlen(wire);
    calls = 0; mcp_stream_init(&stream, 0, message, &id);
    for (at = 0; at < n && !stream.done; at += width) {
        size_t amount = n - at < width ? n - at : width;
        if (mcp_stream_feed(&stream, wire + at, amount) < 0) { fprintf(stderr, "FAIL width=%lu at=%lu status=%d chunk=%d line=%lu msg=%lu\n", (unsigned long)width, (unsigned long)at, stream.status, stream.chunk_state, (unsigned long)stream.line_len, (unsigned long)stream.message_len); assert(0); }
    }
    assert(stream.done && !stream.failed && calls > 0);
}
static void streaming(void)
{
    char wire[4096], data[2048], chunked[4096];
    size_t i, n;
    long id = 1;
    snprintf(wire, sizeof(wire), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %lu\r\nMcp-Session-Id: opaque\r\n\r\n%s", (unsigned long)strlen(response), response);
    for (i = 1; i < 80; i++) fragments(wire, i);
    assert(!strcmp(stream.session, "opaque"));
    snprintf(data, sizeof(data), ": keepalive\r\n\r\ndata:\r\n\r\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}\r\n\r\nevent: message\r\ndata: {\"jsonrpc\":\"2.0\",\r\ndata: \"id\":1,\"result\":{\"text\":\"caf\xc3\xa9\"}}\r\n\r\n");
    snprintf(wire, sizeof(wire), "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n%s", data);
    for (i = 1; i < 90; i++) fragments(wire, i);
    snprintf(wire, sizeof(wire), "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n\xef\xbb\xbf%s", data);
    fragments(wire, 1);
    /* Chunk boundaries inside SSE fields and UTF-8. */
    n = (size_t)snprintf(chunked, sizeof(chunked), "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n");
    for (i = 0; data[i]; i++) n += (size_t)snprintf(chunked + n, sizeof(chunked) - n, "1\r\n%c\r\n", data[i]);
    for (i = 1; i < 90; i++) fragments(chunked, i);
    assert(calls == 2);
    mcp_stream_init(&stream, 1, NULL, NULL);
    strcpy(wire, "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\n\r\n");
    assert(mcp_stream_feed(&stream, wire, strlen(wire)) == 1);
    /* A bodyless acknowledgment is valid whatever Content-Type it carries,
     * but a response that expects a body still needs JSON or SSE. */
    mcp_stream_init(&stream, 1, NULL, NULL);
    strcpy(wire, "HTTP/1.1 202 Accepted\r\nContent-Type: text/plain\r\nContent-Length: 0\r\n\r\n");
    assert(mcp_stream_feed(&stream, wire, strlen(wire)) == 1);
    mcp_stream_init(&stream, 0, message, &id);
    strcpy(wire, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\n{}");
    assert(mcp_stream_feed(&stream, wire, strlen(wire)) < 0);
    mcp_stream_init(&stream, 0, message, &id);
    strcpy(wire, "HTTP/1.1 302 Found\r\nLocation: https://elsewhere/\r\n\r\n");
    assert(mcp_stream_feed(&stream, wire, strlen(wire)) < 0);
    mcp_stream_init(&stream, 0, message, &id);
    strcpy(wire, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Encoding: gzip\r\n\r\n");
    assert(mcp_stream_feed(&stream, wire, strlen(wire)) < 0);
    mcp_stream_init(&stream, 0, message, &id);
    strcpy(wire, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{}");
    assert(mcp_stream_feed(&stream, wire, strlen(wire)) == 0);
    assert(mcp_stream_eof(&stream) < 0);
    mcp_stream_init(&stream, 0, message, &id);
    strcpy(wire, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n");
    assert(mcp_stream_feed(&stream, wire, strlen(wire)) < 0);
    mcp_stream_init(&stream, 0, message, &id);
    stream.traffic = MCP_TRAFFIC_CAP;
    assert(mcp_stream_feed(&stream, "x", 1) < 0);
    mcp_stream_init(&stream, 0, message, &id);
    assert(mcp_stream_feed(&stream, "x", 8193) < 0);
}
int main(void)
{
    configuration(); query_key(); protocol(); discover(); streaming();
    puts("MCP configuration, RPC, registry and fragmented HTTP/SSE tests passed");
    return 0;
}
