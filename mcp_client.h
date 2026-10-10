/* Cooperative Certainly adapter. Open Transport lifecycle belongs to caller.
 * One POST per RPC, no retry, redirect, GET stream or replay. */
#ifndef SHERCLAWK_MCP_CLIENT_H
#define SHERCLAWK_MCP_CLIENT_H
#include "mcp.h"
#include "certainly.h"
enum { MCP_IDLE, MCP_INITIALIZE, MCP_INITIALIZED, MCP_LIST, MCP_READY,
       MCP_CALL, MCP_COMPLETE, MCP_FAILED, MCP_FAIL_DRAIN, MCP_STOPPING, MCP_STOPPED };
typedef struct {
    MacTLS_Context *ctx;
    McpStream stream;
    char request[MCP_MESSAGE_CAP + MCP_HEADER_CAP + 1];
    size_t length, sent;
} McpExchange;
typedef struct {
    McpConfig config;
    McpRegistry registry;
    McpExchange exchange, auxiliary;
    McpWork work;
    char session[1024], version[16], error[160];
    char replies[4][2048], reply[2048];
    char pending[2048]; /* fed bytes behind a message still being processed */
    size_t pending_len;
    int reply_count, state, call_error, cancel_pending, stop_phase, working;
    long id;
    unsigned long started, stop_started;
} McpClient;
int mcp_client_discover(McpClient *c, const McpConfig *config, unsigned long ticks);
int mcp_client_call(McpClient *c, const char *mapped_name, const char *arguments,
                    unsigned long ticks);
/* 0 work remains; 1 ready/complete; -1 failed/stopped. A step either reads at
 * most four 2 KiB pieces of one exchange or processes one received message
 * for at most MCP_WORK_BUDGET units (see mcp.h), never both, so parsing,
 * schema and result work stays interruptible. Deadlines use the caller's
 * wrapping 60 Hz TickCount clock. */
int mcp_client_step(McpClient *c, unsigned long ticks);
void mcp_client_stop(McpClient *c, unsigned long ticks);
/* The verified JSON-RPC response of a completed call, valid until the next
 * call or close; empty in every other state. */
const char *mcp_client_response(const McpClient *c);
/* Returns 1 while an outstanding OT connect must drain. Continue step until
 * terminal before shutting down Open Transport or reusing this client. */
int mcp_client_close(McpClient *c);
#endif
