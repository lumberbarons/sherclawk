/* Remote MCP run controller; see mcp_run.h for the contract. The client is
 * far too large for the classic stack, so it lives here as static state. */
#include "mcp_run.h"
#include "mcp_client.h"
#include "mcp_result.h"
#include "json.h"
#include <stdio.h>
#include <string.h>

static const McpRunHost *host;
static McpClient client;
static McpRunPhase phase = MCP_RUN_IDLE;
/* A stopped or finished client still has to settle and be closed (which
 * wipes its credentials). The event loop steps it through the drain. */
static int close_pending;
static char folder[344];  /* "<workspace>Sherclawk MCP Results:" */
static char alert[320];

void mcp_run_init(const McpRunHost *h)
{
    host = h;
}

McpRunPhase mcp_run_phase(void)
{
    return phase;
}

/* True when the client holds no endpoint that still has to settle, so it
 * needs no more stepping before it can be closed. */
static int endpoints_settled(void)
{
    return client.state != MCP_STOPPING && client.state != MCP_FAIL_DRAIN;
}

/* Journal a sanitized notice: fixed text only, no host, path, header or
 * credential values, matching the editor and diagnostic rules. */
static void notice(const char *detail)
{
    char quoted[288], body[512];
    if (!host->journal || json_quote(detail, quoted, sizeof(quoted)) < 0) return;
    snprintf(body, sizeof(body), "{\"detail\":%s}", quoted);
    host->journal(host->journal_context, "mcp_notice", body);
}

/* One model-facing error result for a call that cannot start or complete.
 * Never retried; no request was sent for the refusal case. */
static void error_result(char *out, size_t cap, const char *code, const char *message)
{
    char quoted[320];
    if (json_quote(message, quoted, sizeof(quoted)) < 0)
        snprintf(quoted, sizeof(quoted), "\"Tool failed\"");
    snprintf(out, cap, "{\"status\":\"error\",\"code\":\"%s\",\"message\":%s}", code, quoted);
}

/* Close the client and wipe its credentials; an outstanding connect moves to
 * the drain instead. */
static void close_client(void)
{
    close_pending = mcp_client_close(&client) != 0;
}

void mcp_run_end(void)
{
    phase = MCP_RUN_IDLE;
    agent_set_mcp_schemas(NULL);
    if (endpoints_settled()) close_client();
    else close_pending = 1;
}

int mcp_run_draining(void)
{
    return close_pending || !endpoints_settled();
}

int mcp_run_drain_pending(void)
{
    return close_pending;
}

/* Step a stopped or failed client until its connects have drained, then
 * close it. */
void mcp_run_drain_step(void)
{
    if (!close_pending) return;
    if (!endpoints_settled()) { mcp_client_step(&client, host->ticks()); return; }
    close_client();
}

int mcp_run_start(void)
{
    static char bytes[MCP_CONFIG_CAP + 1]; /* static: the application stack is small */
    static McpConfig config;               /* credentials live in it until the client copies them */
    char parse_error[160], detail[320];
    McpRunLoad loaded;
    size_t len = 0;

    agent_set_mcp_schemas(NULL); /* each run freezes its own discovery */
    loaded = host->load(bytes, sizeof(bytes), &len);
    if (loaded == MCP_RUN_LOAD_NO_FOLDER) {
        notice("The Preferences folder could not be found; continuing with native tools only.");
        return 0;
    }
    if (loaded == MCP_RUN_LOAD_ABSENT) {
        notice("No MCP configuration is present; continuing with native tools only.");
        return 0;
    }
    if (loaded != MCP_RUN_LOAD_OK) {
        notice(loaded == MCP_RUN_LOAD_RECOVER ?
            "The MCP configuration is unreadable (an interrupted save left only the .old file); continuing with native tools only." :
            "The MCP configuration is unreadable; continuing with native tools only.");
        return 0;
    }
    if (mcp_config_parse(bytes, len, &config, parse_error, sizeof(parse_error))) {
        snprintf(detail, sizeof(detail), "The MCP configuration is invalid (%s); continuing with native tools only.", parse_error);
        notice(detail);
        memset(&config, 0, sizeof(config));
        return 0;
    }
    if (!config.enabled) {
        notice("MCP is disabled in the configuration; continuing with native tools only.");
        memset(&config, 0, sizeof(config));
        return 0;
    }
    /* The retention folder is fixed for the workspace; without it no remote
     * result could ever be handed back, so nothing is admitted. */
    if (snprintf(folder, sizeof(folder), "%s%s:", host->workspace(), MCP_RESULT_FOLDER)
            >= (int)sizeof(folder)) {
        notice("The workspace path is too long for retained MCP results; continuing with native tools only.");
        memset(&config, 0, sizeof(config));
        return 0;
    }
    host->open_transport();
    if (mcp_client_discover(&client, &config, host->ticks()) != 0) {
        snprintf(detail, sizeof(detail), "%s Continuing with native tools only.",
                 client.error[0] ? client.error : "MCP discovery could not start.");
        notice(detail);
        memset(&config, 0, sizeof(config));
        return 0;
    }
    memset(&config, 0, sizeof(config)); /* credentials now live only in the client */
    alert[0] = 0;
    phase = MCP_RUN_DISCOVERING;
    return 1;
}

/* Discovery ends in one of these, and every one reaches the model:
 * admitted tools frozen into the request, a sanitized notice with a
 * native-only fallback, or Stop (which ends the run elsewhere). */
McpDiscovery mcp_run_discover_step(void)
{
    char detail[320], body[256];
    int r = mcp_client_step(&client, host->ticks());
    if (r == 0) return MCP_DISCOVERY_PENDING;
    alert[0] = 0;
    if (r < 0) {
        snprintf(detail, sizeof(detail), "%s Continuing with native tools only.",
                 client.error[0] ? client.error : "MCP discovery was stopped.");
        notice(detail);
        strcpy(alert, detail);
        mcp_run_end();
        return MCP_DISCOVERY_NATIVE;
    }
    if (!client.registry.count) {
        snprintf(detail, sizeof(detail), "%s Continuing with native tools only.",
                 client.registry.notice[0] ? client.registry.notice :
                 "The MCP server offered no eligible tools.");
        notice(detail);
        mcp_run_end();
        return MCP_DISCOVERY_NATIVE;
    }
    if (agent_set_mcp_schemas(client.registry.schemas)) {
        strcpy(detail, "The discovered remote tools could not be admitted (a name collides with a native tool or the request budget); continuing with native tools only.");
        notice(detail);
        strcpy(alert, detail);
        mcp_run_end();
        return MCP_DISCOVERY_NATIVE;
    }
    snprintf(body, sizeof(body), "{\"admitted\":%d,\"bytes\":%lu}",
             agent_mcp_tool_count(), (unsigned long)strlen(client.registry.schemas));
    if (host->journal) host->journal(host->journal_context, "mcp_discovery", body);
    if (client.registry.notice[0]) notice(client.registry.notice);
    phase = MCP_RUN_IDLE; /* the client stays open for the run's calls */
    return MCP_DISCOVERY_ADMITTED;
}

const char *mcp_run_alert(void)
{
    return alert;
}

int mcp_run_admitted(void)
{
    return agent_mcp_tool_count();
}

int mcp_run_owns(const char *name)
{
    return agent_mcp_tool_frozen(name);
}

/* The client forwards the mapped name to its original and the arguments
 * verbatim. A client that cannot take the call (an earlier call failed or was
 * stopped) sends nothing and the model sees an explicit error while the run
 * continues. */
int mcp_run_call(const AgentCall *call, char *out, size_t cap)
{
    if (mcp_client_call(&client, call->name, call->arguments, host->ticks()) == 0) {
        phase = MCP_RUN_CALLING;
        return 1;
    }
    error_result(out, cap, "MCP_UNAVAILABLE",
        "The remote MCP tool could not be called: the connection is closed after an earlier failure or stop. No request was sent and the call was not retried.");
    return 0;
}

int mcp_run_call_step(char *out, size_t cap)
{
    McpResultRetain in;
    const char *response;
    char detail[320];
    int r, started;
    if (phase == MCP_RUN_RETAINING) {
        if (mcp_result_retain_step(out, cap, 0) == 2) return 0;
        phase = MCP_RUN_IDLE;
        return 1;
    }
    if (phase != MCP_RUN_CALLING) {
        error_result(out, cap, "MCP_UNAVAILABLE", "No remote call is in flight.");
        return 1;
    }
    r = mcp_client_step(&client, host->ticks());
    if (r == 0) return 0;
    phase = MCP_RUN_IDLE;
    if (r < 0) {
        /* A failed call is not an abort: the model sees the failure and the
         * run continues natively with the frozen advertisement unchanged. */
        snprintf(detail, sizeof(detail), "%s", client.error[0] ? client.error : "The remote call failed.");
        notice(detail);
        error_result(out, cap, "MCP_UNAVAILABLE", detail);
        return 1;
    }
    /* MCP_COMPLETE: retain the completed, redacted response. */
    response = mcp_client_response(&client);
    memset(&in, 0, sizeof(in));
    in.response = response;
    in.response_len = strlen(response);
    in.folder = folder;
    in.config = &client.config;
    in.session = client.session;
    in.journal = host->journal;
    in.journal_context = host->journal_context;
    started = mcp_result_retain_begin(&in, out, cap);
    if (started == 2) { phase = MCP_RUN_RETAINING; return 0; }
    /* 0/1: an invalid request or a busy/error summary is itself the
     * model-facing result. */
    return 1;
}

int mcp_run_stop(char *out, size_t cap)
{
    McpRunPhase was = phase;
    int recorded = 0;
    if (was == MCP_RUN_IDLE) return 0;
    /* The in-flight remote work concludes first, exactly like a pending
     * native tool: its result is handed back before agent_stop fills the
     * unstarted calls. Closing the client never proves server-side
     * cancellation, so a stopped call says its result is unknown. */
    if (was == MCP_RUN_RETAINING) {
        recorded = mcp_result_retain_step(out, cap, 1) != 2;
        close_client(); /* COMPLETE holds no endpoint; wipe now */
    } else {
        if (was == MCP_RUN_CALLING) {
            error_result(out, cap, "STOPPED",
                "The remote call was stopped before it completed. The server may still finish it; the call was not replayed and its result is unknown.");
            recorded = 1;
        }
        mcp_client_stop(&client, host->ticks());
    }
    if (!endpoints_settled()) close_pending = 1;
    notice("The run was stopped during remote MCP work; nothing was retried.");
    agent_set_mcp_schemas(NULL); /* Stop prevents further remote calls */
    phase = MCP_RUN_IDLE;
    return recorded;
}
