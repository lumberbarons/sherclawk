/* Bounded, allocation-free MCP configuration and protocol core.
 * Credentials stay in McpConfig/session fields, never in schemas or errors. */
#ifndef SHERCLAWK_MCP_H
#define SHERCLAWK_MCP_H
#include <stddef.h>
#include <stdint.h>
#include "json.h"
#define MCP_CONFIG_CAP 8192
#define MCP_HEADER_CAP 8192
#define MCP_MESSAGE_CAP 65536
#define MCP_TRAFFIC_CAP 262144
#define MCP_SCHEMA_CAP 16384
#define MCP_TOOL_MAX 8
#define MCP_ENTRY_MAX 64
#define MCP_PAGE_MAX 8
#define MCP_VERSION "2025-11-25"
#define MCP_CONFIG_FILENAME "Sherclawk MCP Servers.json"
typedef struct {
    int enabled, selection_present, selected_count;
    char server[32], host[256], path[2048];
    uint16_t port;
    char headers[MCP_HEADER_CAP + 1];
    /* Retain values individually for redaction; headers can contain secrets. */
    char secrets[MCP_CONFIG_CAP + 1];
    size_t secrets_len;
    char selected[MCP_TOOL_MAX][64];
    int official_tavily;
} McpConfig;
/* On failure out is disabled and error names a field, never its value. */
int mcp_config_parse(const char *bytes, size_t len, McpConfig *out,
                     char *error, size_t error_cap);
/* In-place, length-preserving credential redaction, including auth payloads. */
void mcp_redact(const McpConfig *config, const char *session, char *text);
typedef struct {
    char name[64], original[64];
} McpTool;
typedef struct {
    McpTool tools[MCP_TOOL_MAX];
    char schemas[MCP_SCHEMA_CAP + 1]; /* comma-separated function objects */
    size_t schema_len;
    int count, pages, entries;
    char seen[MCP_ENTRY_MAX][64];
    char cursor[1024];
    char notice[160];
} McpRegistry;
/* Parse initialization and discovery, ignoring server instructions. */
int mcp_initialize_result(const char *json, char *version, size_t cap);
int mcp_discover_page(const McpConfig *config, McpRegistry *registry,
                      const char *json);
/* Strict RPC classification. Reply id must be the expected numeric id.
 * 1 matching response; 0 notification; 2 server request; -1 malformed/stale.
 * ping requests get a result, others get -32601, using their original id. */
int mcp_rpc(const char *json, long expected_id, char *reply, size_t reply_cap);
int mcp_post(const McpConfig *config, const char *session, const char *version,
             const char *body, char *out, size_t cap);
/* HTTP/SSE incremental decoder, independent of TLS. Stops at matching RPC
 * response without requiring the stream to close. Each feed is <= 8 KiB.
 * callback: 1 done, 0 continue, -1 failure. */
typedef int (*McpMessage)(void *context, const char *json);
typedef struct {
    char headers[MCP_HEADER_CAP + 1];
    char line[MCP_MESSAGE_CAP + 1], message[MCP_MESSAGE_CAP + 1];
    char session[1024], chunk_line[MCP_HEADER_CAP + 1];
    size_t chunk_len;
    size_t header_len, line_len, message_len, traffic, remaining, chunk_left;
    int headers_done, status, sse, chunked, has_length, chunk_state;
    int done, failed, acknowledgment, skip_lf, bom;
    McpMessage callback;
    void *context;
} McpStream;
void mcp_stream_init(McpStream *s, int acknowledgment, McpMessage callback, void *context);
int mcp_stream_feed(McpStream *s, const char *bytes, size_t len);
int mcp_stream_eof(McpStream *s);
#endif
