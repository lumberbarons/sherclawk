/* Retained MCP results: immutable verified artifacts with session-scoped
 * IDs, paged to the model by read_mcp_result. The adapter owns the whole
 * lifecycle (decode, stage, verify, publish, page); see docs/tools.md and
 * docs/limits.md. Input responses must be completed, redacted call responses
 * (mcp_client_response); MCP_STAGE_CALL owns sanitization, and retention
 * re-checks the decoded text with McpRedact before it is written. */
#ifndef SHERCLAWK_MCP_RESULT_H
#define SHERCLAWK_MCP_RESULT_H
#include <stddef.h>
#include "agent.h"
#include "mcp.h"

/* Retained UTF-8 bytes per artifact. A response is at most MCP_MESSAGE_CAP,
 * so this is headroom above the largest possible input. */
#define MCP_RESULT_CAP 131072
/* Retained results per session; the next retention fails with LIMIT. */
#define MCP_RESULT_MAX 16
/* Raw UTF-8 bytes per read_mcp_result page; a page never splits a multi-byte
 * character, and its JSON-escaped form fits AGENT_RESULT_CAP. */
#define MCP_RESULT_PAGE 512
/* Source links surfaced with the retention summary. */
#define MCP_RESULT_LINK_MAX 6
#define MCP_RESULT_LINK_LEN 300
/* Workspace folder the artifacts are staged and published in. */
#define MCP_RESULT_FOLDER "Sherclawk MCP Results"
#define MCP_RESULT_CREATOR 0x5368436bUL /* 'ShCk' */
#define MCP_RESULT_TYPE 0x55544638UL    /* 'UTF8' */

/* One retention request. `folder` is the full colon path (ending in ':') of
 * MCP_RESULT_FOLDER under the workspace; it is created when absent. `config`
 * and `session` re-run credential redaction over the decoded text and may be
 * NULL when no credentials are configured. `journal` may be NULL. */
typedef struct {
    const char *response;
    size_t response_len;
    const char *folder;
    const McpConfig *config;
    const char *session;
    AgentJournal journal;
    void *journal_context;
} McpResultRetain;

/* Retain `response` as a verified artifact and publish its session-scoped
 * ID. Returns 2 with retention started, 0 with an error summary (invalid
 * request, unusable folder, full table), or 1 when another retention is
 * already active. `out` always holds a result when returning 0 or 1. */
int mcp_result_retain_begin(const McpResultRetain *in, char *out, size_t cap);
/* Advance the active retention: 2 while work remains, 0 with the retention
 * summary in `out`, or 1 with an error summary (failure, Stop, or a session
 * change). Failure never advertises the artifact; Stop deletes the stage and
 * the call is not replayed. */
int mcp_result_retain_step(char *out, size_t cap, int stop);
/* The read_mcp_result executor: one bounded page of a verified artifact. */
int mcp_result_read(const AgentCall *call, char *out, size_t cap);
/* New journal, new session: every retained ID becomes stale. Journal records
 * are append-only evidence; nothing is ever restored from them. */
void mcp_result_new_session(void);
#endif
