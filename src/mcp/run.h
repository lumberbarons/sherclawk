/* Remote MCP tools in the agent run loop, apart from the Toolbox UI. Each
 * run loads the saved configuration before its first model request,
 * discovers the server's eligible tools and freezes the admitted schemas into
 * the request advertisement (agent_set_mcp_schemas). Model-selected calls are
 * dispatched one at a time by their exact frozen names; each completed,
 * redacted response is retained as a verified artifact and the retention
 * summary is the model-facing result, paged back by read_mcp_result.
 * ADR-0003 keeps admission read-only: a missing, disabled or invalid
 * configuration and a failed or over-budget discovery cause no remote
 * traffic (or none beyond the failed discovery) and a native-only run with a
 * sanitized notice. Stop stops the client, drains its outstanding connects
 * cooperatively and clears the freeze, so no further remote call is
 * dispatched and nothing is replayed. Every step is bounded; the caller owns
 * the event loop, the run state and Open Transport. */
#ifndef SHERCLAWK_MCP_RUN_H
#define SHERCLAWK_MCP_RUN_H
#include <stddef.h>
#include "agent/agent.h"

typedef enum {
    MCP_RUN_LOAD_OK,
    MCP_RUN_LOAD_ABSENT,      /* no configuration file yet */
    MCP_RUN_LOAD_NO_FOLDER,   /* the Preferences folder could not be found */
    MCP_RUN_LOAD_RECOVER,     /* an interrupted save left only the .old file */
    MCP_RUN_LOAD_UNREADABLE   /* any other load failure */
} McpRunLoad;

/* What the controller needs from the application. `journal` receives every
 * sanitized notice and the retention events; its context is passed back. */
typedef struct {
    McpRunLoad (*load)(char *bytes, size_t cap, size_t *len);
    const char *(*workspace)(void);   /* colon path of the workspace, ending in ':' */
    void (*open_transport)(void);     /* idempotent; before the first remote connect */
    unsigned long (*ticks)(void);     /* wrapping 60 Hz clock */
    AgentJournal journal;
    void *journal_context;
} McpRunHost;

typedef enum {
    MCP_RUN_IDLE,
    MCP_RUN_DISCOVERING,
    MCP_RUN_CALLING,
    MCP_RUN_RETAINING
} McpRunPhase;

typedef enum {
    MCP_DISCOVERY_PENDING,
    MCP_DISCOVERY_ADMITTED,   /* schemas frozen; mcp_run_admitted() tools */
    MCP_DISCOVERY_NATIVE      /* continue with native tools only */
} McpDiscovery;

void mcp_run_init(const McpRunHost *host);
McpRunPhase mcp_run_phase(void);

/* Load the configuration and start discovery. Returns 1 with the phase
 * DISCOVERING, or 0 to continue the run natively (a notice was journaled). */
int mcp_run_start(void);
/* Advance discovery. On NATIVE, mcp_run_alert() is non-empty when the user
 * should also be shown the notice. */
McpDiscovery mcp_run_discover_step(void);
const char *mcp_run_alert(void);
int mcp_run_admitted(void);

/* True when `name` is a frozen remote tool of this run. */
int mcp_run_owns(const char *name);
/* Dispatch one model-selected call with its arguments verbatim, opening Open
 * Transport first (the application closes it between rounds). Returns 1 with
 * the phase CALLING, or 0 with an error result in `out` (the client cannot
 * take the call; nothing was sent and nothing is retried). */
int mcp_run_call(const AgentCall *call, char *out, size_t cap);
/* Advance the call through retention. Returns 0 while work remains (the
 * phase moves to RETAINING once a response arrives) or 1 with the tool's
 * model-facing result in `out`; the phase is then IDLE. */
int mcp_run_call_step(char *out, size_t cap);

/* Stop remote work. Returns 1 when `out` holds the result of the interrupted
 * call, which the caller must record; 0 when nothing was in flight. Clears
 * the frozen advertisement and leaves any connect to the drain. */
int mcp_run_stop(char *out, size_t cap);
/* The run is over (completion, Stop or New Chat): clear the advertisement
 * and wipe the client's credentials, deferring to the drain when a connect
 * is outstanding. */
void mcp_run_end(void);

/* True while the client needs stepping or holds an open endpoint: Open
 * Transport may not be torn down and no run may start. */
int mcp_run_draining(void);
/* A close is deferred until the client settles; the event loop steps it. */
int mcp_run_drain_pending(void);
void mcp_run_drain_step(void);
#endif
