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
/* In-place, length-preserving credential redaction, including auth payloads.
 * mcp_redact runs to completion; McpRedact is the same search as resumable
 * steps of at most `budget` units (one text/needle comparison each), and
 * mcp_redact_step returns 1 once finished. */
void mcp_redact(const McpConfig *config, const char *session, char *text);
typedef struct {
    const McpConfig *config;
    char *text;
    const char *needle;
    int state, stage;
    size_t at, scan, space, pos, k, work;
} McpRedact;
void mcp_redact_init(McpRedact *r, const McpConfig *config, const char *session, char *text);
int mcp_redact_step(McpRedact *r, size_t budget);
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
/* Resumable processing of one complete JSON-RPC message in steps of at most
 * MCP_WORK_BUDGET units (see json.h): parse, duplicate-key check, then any of
 * the stages below over the same tokens. Units count input bytes, tokens,
 * table slots and copied bytes, so a step is bounded by message size rather
 * than by how hostile the message is. Messages wait in the stream until the
 * work finishes; Stop simply abandons the work. */
#define MCP_WORK_BUDGET 8192
enum {
    MCP_STAGE_CLASSIFY = 1,   /* strict RPC classification (rpc below) */
    MCP_STAGE_INITIALIZE = 2, /* initialize result: version and capabilities */
    MCP_STAGE_DISCOVER = 4,   /* one tools/list page into the registry */
    MCP_STAGE_CALL = 8        /* tool-call result flags */
};
typedef struct {
    int stages, active, phase, next, fail_phase, tool, list;
    const char *fail_reason;
    const McpConfig *config;
    McpRegistry *registry;
    const char *text;
    size_t len;
    long expected;
    char *reply, *version;
    size_t reply_cap, version_cap;
    JsonParser parser;
    JsonUnique unique;
    JsonPick pick;
    JsonDecode decode;
    McpRedact redact;
    int root[JSON_PICK_MAX], held[JSON_PICK_MAX];
    int dup, piece, read_only;
    size_t desc_len, quoted_at, quoted_used, prefix_len, piece_at, copied, total;
    /* Results. rpc: 1 matching response, 0 notification, 2 server request
     * (reply holds the answer), -1 malformed/stale. status: 0 accepted, -1
     * refused by the initialize/discover/call stage. */
    int rpc, status, more, call_error;
    /* Measurements: units of the latest and the busiest step, and in all. */
    size_t work, peak, units;
    unsigned long steps;
    char name[64], mapped[64], prefix[160], scratch[1024];
    /* Everything above is cleared by mcp_work_init; the buffers below are
     * written before they are read. */
    char description[8192], quoted[16384];
    uint16_t slots[JSON_UNIQUE_SLOTS];
    JsonToken tokens[8192];
} McpWork;
/* version/reply may be NULL when their stage is not requested. For
 * MCP_STAGE_CLASSIFY with an operation stage, the operation only runs when the
 * message is the matching response. The McpConfig and registry must outlive
 * the work. */
void mcp_work_init(McpWork *w, int stages, const McpConfig *config, McpRegistry *registry,
                   const char *text, size_t len, long expected_id,
                   char *reply, size_t reply_cap, char *version, size_t version_cap);
/* 1 when finished (read rpc/status), 0 when more work remains. */
int mcp_work_step(McpWork *w, size_t budget);
int mcp_post(const McpConfig *config, const char *session, const char *version,
             const char *body, char *out, size_t cap);
/* HTTP/SSE incremental decoder, independent of TLS. Every byte costs constant
 * work and each feed is <= 8 KiB. A complete message is not processed here:
 * mcp_stream_feed/eof return 2 and leave it in `message` (bytes of the feed
 * from `bytes + used` on are unconsumed and must be fed again). Process it,
 * then mcp_stream_resume: matched finishes the stream, otherwise an SSE stream
 * goes on to the next event and a plain JSON body fails. */
typedef struct {
    char headers[MCP_HEADER_CAP + 1];
    char message[MCP_MESSAGE_CAP + 1];
    char chunk_line[MCP_HEADER_CAP + 1];
    /* mcp_stream_init clears everything from here on. */
    char session[1024], field[16];
    size_t chunk_len;
    size_t header_len, line_len, message_len, traffic, remaining, chunk_left, used;
    int headers_done, status, sse, chunked, has_length, chunk_state;
    int done, failed, ready, acknowledgment, skip_lf, bom;
    int field_len, in_value, data_field, skip_space;
} McpStream;
void mcp_stream_init(McpStream *s, int acknowledgment);
/* -1 failure, 0 need more, 1 done, 2 message ready. */
int mcp_stream_feed(McpStream *s, const char *bytes, size_t len);
int mcp_stream_eof(McpStream *s);
void mcp_stream_resume(McpStream *s, int matched);
#endif
