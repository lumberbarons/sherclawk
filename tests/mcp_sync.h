/* Run-to-completion helpers over McpWork for host fixtures. Every step is held
 * to its budget; the work itself is what the app steps from its event loop. */
#ifndef SHERCLAWK_TEST_MCP_SYNC_H
#define SHERCLAWK_TEST_MCP_SYNC_H
#include "mcp.h"
#include <assert.h>
#include <string.h>

static McpWork sync_work;

static void sync_run(size_t budget)
{
    while (!mcp_work_step(&sync_work, budget)) assert(sync_work.work <= budget || budget < JSON_PICK_NEED);
    assert(sync_work.work <= budget || budget < JSON_PICK_NEED);
}
/* 1 matching response, 0 notification, 2 server request, -1 malformed/stale. */
static int rpc(const char *s, long expected, char *reply, size_t cap)
{
    mcp_work_init(&sync_work, MCP_STAGE_CLASSIFY, NULL, NULL, s, strlen(s), expected, reply, cap, NULL, 0);
    sync_run(MCP_WORK_BUDGET);
    return sync_work.rpc;
}
static int initialize_result(const char *s, char *version, size_t cap)
{
    mcp_work_init(&sync_work, MCP_STAGE_INITIALIZE, NULL, NULL, s, strlen(s), 0, NULL, 0, version, cap);
    sync_run(MCP_WORK_BUDGET);
    return sync_work.status;
}
/* 1 last page, 0 more pages, -1 refused. */
static int discover_with(const McpConfig *c, McpRegistry *r, const char *s, size_t budget)
{
    mcp_work_init(&sync_work, MCP_STAGE_DISCOVER, c, r, s, strlen(s), 0, NULL, 0, NULL, 0);
    sync_run(budget);
    return sync_work.status < 0 ? -1 : sync_work.more ? 0 : 1;
}
static int discover_page(const McpConfig *c, McpRegistry *r, const char *s)
{
    return discover_with(c, r, s, MCP_WORK_BUDGET);
}
#endif
