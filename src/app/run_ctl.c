/* Run orchestration, independent of Toolbox controls. Network/OT calls and
 * presentation cross RunHost; protocol, journal barriers and tool dispatch are
 * the production implementations shared with the guest. */
#include "app/run_ctl.h"
#include "app/display.h"
#include "agent/json.h"
#include "agent/text.h"
#include "tools/tools.h"
#include "tools/view_image.h"
#include "native/build_project.h"
#include "native/run_application.h"
#include "mcp/run.h"
#include "http.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

void run_ctl_init(RunCtl *ctl, const RunHost *host, void *context,
                  Agent *agent, Session *session, Prefs *prefs, ChatNetwork *net)
{
    memset(ctl, 0, sizeof(*ctl));
    ctl->host = host; ctl->context = context;
    ctl->agent = agent; ctl->session = session; ctl->prefs = prefs; ctl->net = net;
}
int run_ctl_busy(const RunCtl *ctl) { return ctl->state != RUN_IDLE; }
static void SetStatus(RunCtl *ctl, const char *fmt, ...)
{
    char text[256]; va_list args;
    va_start(args, fmt); vsnprintf(text, sizeof(text), fmt, args); va_end(args);
    ctl->host->set_status(ctl->context, text);
}
static void SetSendEnabled(RunCtl *ctl, int on) { ctl->host->set_send_enabled(ctl->context, on); }
static void ShowMessage(RunCtl *ctl, const char *label, const char *text) { ctl->host->show_message(ctl->context, label, text); }
static void ShowToolResult(RunCtl *ctl, const AgentCall *call, const char *label, const char *text)
{
    ctl->host->show_tool_result(ctl->context, call, label, text);
}
static void LogLine(RunCtl *ctl, const char *text) { ctl->host->log_line(ctl->context, text); }
int run_ctl_tool_journal(void *context, const char *event, const char *json)
{
    RunCtl *ctl = context;
    ToolEventsAppend(ctl->tool_events, sizeof(ctl->tool_events), event);
    return session_journal(ctl->session, event, json);
}
static int StartModelRequest(RunCtl *ctl);
static void SendAdvance(RunCtl *ctl);
static const char *TLSVersionLabel(MacTLS_Version v)
{
    switch (v) {
    case kMacTLS_Version13: return "TLS 1.3";
    case kMacTLS_Version12: return "TLS 1.2";
    default:                return "TLS ?";
    }
}
/* Fully cycle OT between exchanges: failures can wedge the classic stack.
 * Keep the existing yield on each side of CloseOpenTransport (default 10
 * ticks, guest-soaked in #25). MCP connects defer only the OT close until
 * their drain is terminal; the model context is closed now. */
void run_ctl_close_context(RunCtl *ctl)
{
    ctl->host->close_ctx(ctl->context, ctl->net);
    ctl->host->yield_transport(ctl->context);
    if (ctl->ot_open) {
        /* The MCP client's endpoints live in the same OT instance: tearing
         * it down while one is still connecting is the classic wedge. The
         * drain in the event loop finishes the teardown once the client is
         * terminal (DrainMcpClient). */
        if (mcp_run_draining()) ctl->ot_close_pending = 1;
        else { ctl->host->close_transport(ctl->context); ctl->ot_open = 0; ctl->ot_close_pending = 0; }
    }
    ctl->host->yield_transport(ctl->context);
}

/* Never tear down an outstanding async connect/handshake. Stop drains by
 * pumping only, without writing the prompt or reading a response. */
static int ExchangeConnecting(RunCtl *ctl)
{
    MacTLS_State state;
    if (!ctl->net->ctx || ctl->net->result) return 0;
    state = ctl->host->network_state(ctl->context, ctl->net);
    return state == kMacTLS_Idle || state == kMacTLS_Connecting ||
           state == kMacTLS_Handshaking;
}

void run_ctl_begin_drain(RunCtl *ctl)
{
    if (ctl->exchange_drain) return;
    ctl->exchange_drain = 1;
    ctl->drain_start_ticks = ctl->host->tick_count(ctl->context);
}

void run_ctl_abandon_context(RunCtl *ctl)
{
    if (ExchangeConnecting(ctl)) { run_ctl_begin_drain(ctl); return; }
    if (ctl->net->ctx || ctl->ot_open) run_ctl_close_context(ctl);
}

/* Only a clean completed round may retain OT under the optional keep-open
 * setting; aborts and quit always abandon through the connect drain. */
static void FinishChatContext(RunCtl *ctl)
{
#if SHERCLAWK_OT_KEEP_OPEN_AFTER_CLEAN
    ctl->host->close_ctx(ctl->context, ctl->net);
    ctl->host->yield_transport(ctl->context);
#else
    run_ctl_close_context(ctl);
#endif
}

void run_ctl_open_transport(RunCtl *ctl)
{
    if (!ctl->ot_open) { ctl->host->open_transport(ctl->context); ctl->ot_open = 1; }
}

void run_ctl_log_round(RunCtl *ctl, const char *outcome)
{
    char line[256];
    if (!ctl->round_timing.active) return;
    timing_mark(&ctl->round_timing, TIMING_CLOSE, ctl->host->tick_count(ctl->context));
    timing_set_bytes(&ctl->round_timing, (unsigned long)ctl->net->sent, (unsigned long)ctl->net->received);
    if (timing_round_format(&ctl->round_timing, outcome, line, sizeof(line)) > 0) LogLine(ctl, line);
    ctl->round_timing.active = 0;
}

static void LogToolTiming(RunCtl *ctl, int index, const char *name)
{
    char line[96];
    if (timing_tool_format(index, name, ctl->tool_start, ctl->host->tick_count(ctl->context), line, sizeof(line)) > 0) LogLine(ctl, line);
}

/* Provider usage is observed even for a response agent_response rejects. */
static void ObserveCompletionTokens(RunCtl *ctl)
{
    long completion, reasoning;
    if (!ctl->round_timing.active || agent_usage_completion(ctl->net->body, ctl->net->body_len, &completion, &reasoning)) return;
    timing_set_tokens(&ctl->round_timing, completion, reasoning, ctl->state == RUN_HANDOFF ? AGENT_HANDOFF_MAX_TOKENS : AGENT_MAX_TOKENS);
}

static void ObserveRound(RunCtl *ctl)
{
    uint32_t now = ctl->host->tick_count(ctl->context);
    MacTLS_State state;
    if (!ctl->net->ctx || !ctl->round_timing.active) return;
    state = ctl->host->network_state(ctl->context, ctl->net);
    if (state == kMacTLS_Handshaking || state == kMacTLS_Connected) timing_mark(&ctl->round_timing, TIMING_CONNECT, now);
    if (state == kMacTLS_Connected) timing_mark(&ctl->round_timing, TIMING_HANDSHAKE, now);
    if (ctl->net->request_len && ctl->net->sent == ctl->net->request_len) timing_mark(&ctl->round_timing, TIMING_SENT, now);
    if (ctl->net->received) timing_mark(&ctl->round_timing, TIMING_FIRST_BYTE, now);
    if (ctl->net->result) timing_mark(&ctl->round_timing, TIMING_DONE, now);
}

/* Finish the OT close held back by the stopped MCP client's connect drain. */
void run_ctl_drain_mcp(RunCtl *ctl)
{
    if (!mcp_run_drain_pending()) return;
    mcp_run_drain_step();
    if (!mcp_run_drain_pending() && ctl->ot_close_pending && !ctl->net->ctx) {
        ctl->host->close_transport(ctl->context);
        ctl->ot_open = 0; ctl->ot_close_pending = 0;
    }
}

static void McpToolDone(RunCtl *ctl)
{
    char error[256];
    const AgentCall *call = &ctl->agent->calls[ctl->agent->next];
    ctl->state = RUN_TOOLS;
    LogToolTiming(ctl, ctl->agent->next + 1, call->name);
    if (agent_tool_result(ctl->agent, ctl->tool_result, error, sizeof(error))) { run_ctl_abort(ctl, error); return; }
    ShowToolResult(ctl, call, call->name, ctl->tool_result);
}

static void StepMcpDiscovery(RunCtl *ctl)
{
    McpDiscovery found = mcp_run_discover_step();
    if (found == MCP_DISCOVERY_PENDING) return;
    if (found == MCP_DISCOVERY_ADMITTED)
        SetStatus(ctl, "%d remote MCP tool(s) available; contacting the model...", mcp_run_admitted());
    else if (*mcp_run_alert()) ShowMessage(ctl, "Notice", mcp_run_alert());
    /* Discovery is over: an early abort in StartModelRequest is not MCP work. */
    ctl->state = RUN_NEXT_REQUEST;
    StartModelRequest(ctl);
}

static void StartMcpCall(RunCtl *ctl, AgentCall *call)
{
    if (mcp_run_call(call, ctl->tool_result, sizeof(ctl->tool_result))) {
        ctl->state = RUN_MCP;
        SetStatus(ctl, "Calling remote tool %s (%d/%d)...", call->name, ctl->agent->next + 1, ctl->agent->count);
        return;
    }
    McpToolDone(ctl);
}

static void StepMcp(RunCtl *ctl)
{
    McpRunPhase before = mcp_run_phase();
    if (before == MCP_RUN_DISCOVERING) { StepMcpDiscovery(ctl); return; }
    if (mcp_run_call_step(ctl->tool_result, sizeof(ctl->tool_result))) { McpToolDone(ctl); return; }
    if (before == MCP_RUN_CALLING && mcp_run_phase() == MCP_RUN_RETAINING)
        SetStatus(ctl, "Retaining remote result...");
}
/* Long-running tools finish over many event-loop steps. A row here pairs a
 * registry row with its run state, start and step functions and messages;
 * Stop and normal completion both finish it through PendingToolStep. A new
 * one is a registry row plus a RunState value plus a row here. */
typedef struct {
    RunState state;
    const ToolDef *def;   /* registry row that names the tool */
    /* NULL delegates to tools_execute_checked, which reuses preflight tokens. */
    int (*begin)(const AgentCall *, char *, size_t, AgentJournal, void *, uint32_t);
    int (*step)(char *, size_t, uint32_t, int stop);
    const char *status;  /* shown while it is pending */
    const char *failure; /* stop reason when the step reports failure */
} PendingState;
static const PendingState kPendingStates[] = {
    { RUN_READ_TEXT, &kToolDefs[TOOL_read_text], NULL, read_text_step,
      "Reading and verifying text...", "Text read stopped; no revision supplied." },
    { RUN_EDIT_TEXT, &kToolDefs[TOOL_edit_text], NULL, edit_text_step,
      "Staging and verifying exact edit...", "Edit stopped. Inspect retained recovery paths before continuing." },
    { RUN_BUILD, &kToolDefs[TOOL_build_project], build_project_begin, build_project_step,
      "Building snapshot with MPW ToolServer...",
      "Build observation stopped. Inspect retained snapshot and logs before another build." },
    { RUN_LAUNCH, &kToolDefs[TOOL_run_application], run_application_begin, run_application_step,
      "Verifying built application before launch...",
      "Launch outcome uncertain. Inspect the run journal; do not retry automatically." },
    { RUN_QUIT, &kToolDefs[TOOL_quit_application], quit_application_begin, quit_application_step,
      "Requesting graceful quit; observing owned process...",
      "Quit outcome uncertain. The request may still take effect; do not retry." },
    { RUN_VIEW_IMAGE, &kToolDefs[TOOL_view_image], view_image_begin, view_image_step,
      "Reading image...",
      "Image read stopped. Nothing was attached." },
    { RUN_COPY_FILE, &kToolDefs[TOOL_copy_file], NULL, copy_file_step,
      "Copying and verifying both forks...",
      "Copy stopped. Inspect the retained stage and session recovery records before continuing." }
};
static const PendingState *PendingStateForDef(const ToolDef *def)
{
    size_t i;
    for (i = 0; i < sizeof(kPendingStates) / sizeof(kPendingStates[0]); i++)
        if (kPendingStates[i].def == def) return &kPendingStates[i];
    return NULL;
}
static const PendingState *PendingStateForRun(RunState state)
{
    size_t i;
    for (i = 0; i < sizeof(kPendingStates) / sizeof(kPendingStates[0]); i++)
        if (kPendingStates[i].state == state) return &kPendingStates[i];
    return NULL;
}
/* Advance the pending tool; `stop` makes it conclude now. Returns 2 while it
 * is still running. Otherwise the run returns to RUN_TOOLS (before recording,
 * so a recording failure that aborts cannot finish the tool twice), the result
 * is recorded and shown, and 0 comes back with the tool's own status in
 * *result, or -1 with `error` if the result could not be recorded. */
static int PendingToolStep(RunCtl *ctl, const PendingState *tool, int stop, int *result, char *error, size_t cap)
{
    const AgentCall *call = ctl->agent->next < ctl->agent->count ? &ctl->agent->calls[ctl->agent->next] : NULL;
    *result = tool->step(ctl->tool_result, sizeof(ctl->tool_result), ctl->host->tick_count(ctl->context), stop);
    if (!stop && *result == 2) return 2;
    ctl->state = RUN_TOOLS;
    if (!stop) LogToolTiming(ctl, ctl->agent->next + 1, tool->def->name);
    if (agent_tool_result(ctl->agent, ctl->tool_result, error, cap)) return -1;
    ShowToolResult(ctl, call, tool->def->name, ctl->tool_result);
    return 0;
}

void run_ctl_handoff(RunCtl *ctl)
{
    int length;
    char attribution[256];
    if (run_ctl_busy(ctl)) return;
    if (ctl->exchange_drain) { SetStatus(ctl, "The stopped request is still closing; try again in a moment."); return; }
    if (mcp_run_draining()) { SetStatus(ctl, "The stopped MCP connection is still closing; try again in a moment."); return; }
    if (!ctl->session->open || !ctl->agent->messages || ctl->agent->active || ctl->agent->next < ctl->agent->count) {
        SetStatus(ctl, "Finish or stop the current run before saving a handoff."); return;
    }
    strcpy(ctl->model, ctl->prefs->model);
    if (!ctl->prefs->api_key[0] || !*ctl->model) {
        SetStatus(ctl, "A model and API key are needed for a handoff."); return;
    }
    if (*ctl->model && !model_id_valid(ctl->model)) {
        SetStatus(ctl, "Use an OpenRouter model ID without spaces."); return;
    }
    length = agent_handoff_request(ctl->agent, ctl->model, ctl->prefs->effort, ctl->json, sizeof(ctl->json));
    if (length < 0) { SetStatus(ctl, "Could not prepare handoff; conversation retained."); return; }
    if (sherclawk_attribution(SHERCLAWK_APP_URL, attribution, sizeof(attribution)) < 0) {
        SetStatus(ctl, "Attribution URL is too long; conversation retained."); return;
    }
    length = http_build_post_with_headers("openrouter.ai", "/api/v1/chat/completions", ctl->prefs->api_key,
        attribution, ctl->json, (size_t)length, ctl->net->request, sizeof(ctl->net->request));
    if (length < 0) { SetStatus(ctl, "Could not prepare handoff request; conversation retained."); return; }
    ctl->handoff_path[0] = 0; ctl->state = RUN_HANDOFF;
    timing_round_begin(&ctl->round_timing, 0, ctl->host->tick_count(ctl->context));
    run_ctl_open_transport(ctl); ctl->start_ticks = ctl->host->tick_count(ctl->context);
    timing_mark(&ctl->round_timing, TIMING_INIT, ctl->start_ticks);
    SetSendEnabled(ctl, 0);
    if (ctl->host->network_start(ctl->context, ctl->net, ctl->net->request, (size_t)length) < 0) { run_ctl_abort(ctl, ctl->net->error); return; }
    SetStatus(ctl, "Summarizing for handoff; current conversation retained...");
}

static void LogAbort(RunCtl *ctl, const char *prefix, const char *reason)
{
    char line[320];
    snprintf(line, sizeof(line), "%s: %s", prefix, reason);
    LogLine(ctl, line);
}

void run_ctl_abort(RunCtl *ctl, const char *reason)
{
    const PendingState *pending = PendingStateForRun(ctl->state);
    if (ctl->state == RUN_HANDOFF) {
        run_ctl_abandon_context(ctl);
        run_ctl_log_round(ctl, "abort");
        ctl->state = RUN_IDLE; SetSendEnabled(ctl, 1); ctl->host->focus_prompt(ctl->context);
        ShowMessage(ctl, "Handoff stopped; conversation retained", reason);
        if (*ctl->handoff_path) ShowMessage(ctl, "Retained handoff file (may be incomplete)", ctl->handoff_path);
        SetStatus(ctl, "Handoff stopped; conversation retained. %s", reason);
        LogAbort(ctl, "Handoff stopped; conversation retained. Reason", reason);
        return;
    }
    if (ctl->state == RUN_CONTEXT_LOOKUP) {
        run_ctl_abandon_context(ctl);
        ctl->state = RUN_IDLE; SetSendEnabled(ctl, 1); ctl->host->focus_prompt(ctl->context);
        SetStatus(ctl, "Model context lookup stopped. Send again shortly.");
        LogAbort(ctl, "Model context lookup stopped. Reason", reason);
        return;
    }
    if (ctl->state == RUN_MCP) {
        char error[256];
        /* The in-flight remote work concludes first, exactly like a pending
         * native tool: its result is recorded before agent_stop fills the
         * unstarted calls. */
        if (mcp_run_stop(ctl->tool_result, sizeof(ctl->tool_result))) {
            LogToolTiming(ctl, ctl->agent->next + 1, ctl->agent->calls[ctl->agent->next].name);
            if (agent_tool_result(ctl->agent, ctl->tool_result, error, sizeof(error))) {
                SetStatus(ctl, "Session recording failed. Start a new session before continuing.");
                session_close(ctl->session);
            }
        }
    }
    if (pending) {
        char error[256];
        int result;
        PendingToolStep(ctl, pending, 1, &result, error, sizeof(error));
    }
    /* Every run abort ends the run's remote state, whatever stopped it (a
     * model error, the time or round limit, Stop between calls, quit): the
     * client's endpoints close before Open Transport can be torn down and its
     * credentials are wiped now, not at the next run. A no-op when no MCP
     * client is open. */
    mcp_run_end();
    run_ctl_abandon_context(ctl);
    run_ctl_log_round(ctl, "abort");
    if (agent_stop(ctl->agent, reason)) {
        SetStatus(ctl, "Session recording failed. Start a new session before continuing.");
        session_close(ctl->session);
    } else SetStatus(ctl, "%s", reason);
    ctl->state = RUN_IDLE; SetSendEnabled(ctl, 1); ctl->host->focus_prompt(ctl->context);
    ShowMessage(ctl, "Stopped", reason);
    LogAbort(ctl, "Agent stopped; completed records retained. Reason", reason);
}

static void PauseRunAtLimit(RunCtl *ctl)
{
    char reason[256];
    FormatPauseReason(ctl->agent->rounds, ctl->agent->tool_count, ctl->prefs->max_rounds,
                      ctl->prefs->max_tools, ctl->agent->used, reason, sizeof(reason));
    run_ctl_abort(ctl, reason);
}

/* Bad responses are rejected before entering history. Keep HTTP status,
 * body size and the reason in the journal without retaining the body. */
static void JournalModelError(RunCtl *ctl, const char *reason)
{
    static char quoted[1600], detail[1700];
    if (!ctl->session->open || json_quote(reason, quoted, sizeof(quoted)) < 0) return;
    snprintf(detail, sizeof(detail), "{\"http_status\":%d,\"received_bytes\":%lu,\"error\":%s}",
        ctl->net->status, (unsigned long)ctl->net->body_len, quoted);
    session_journal(ctl->session, "model_error", detail);
}

static int StartModelRequest(RunCtl *ctl)
{
    int length;
    char attribution[256];
    if (ctl->agent->rounds >= ctl->prefs->max_rounds || ctl->agent->tool_count >= ctl->prefs->max_tools) {
        PauseRunAtLimit(ctl); return -1;
    }
    length = agent_request_image(ctl->agent, ctl->model, ctl->prefs->effort, view_image_held(), ctl->json, sizeof(ctl->json));
    if (length < 0) { run_ctl_abort(ctl, "Request limit reached. Start a new session."); return -1; }
    if (sherclawk_attribution(SHERCLAWK_APP_URL, attribution, sizeof(attribution)) < 0) {
        run_ctl_abort(ctl, "Attribution URL is too long."); return -1;
    }
    length = http_build_post_with_headers("openrouter.ai", "/api/v1/chat/completions", ctl->prefs->api_key,
        attribution, ctl->json, (size_t)length, ctl->net->request, sizeof(ctl->net->request));
    if (length < 0) { run_ctl_abort(ctl, "Request or API key is too long or invalid."); return -1; }
    timing_round_begin(&ctl->round_timing, ctl->agent->rounds + 1, ctl->host->tick_count(ctl->context));
    run_ctl_open_transport(ctl);
    ctl->start_ticks = ctl->host->tick_count(ctl->context); ctl->state = RUN_MODEL_REQUEST;
    timing_mark(&ctl->round_timing, TIMING_INIT, ctl->start_ticks);
    if (ctl->host->network_start(ctl->context, ctl->net, ctl->net->request, (size_t)length) < 0) { run_ctl_abort(ctl, ctl->net->error); return -1; }
    SetStatus(ctl, "Connecting to OpenRouter (round %d)...", ctl->agent->rounds + 1);
    LogLine(ctl, "Agent model request started."); return 0;
}

/* Session/history changes happen only after the optional context lookup,
 * so stopping that lookup preserves the typed prompt and creates no record. */
static void SendBegin(RunCtl *ctl)
{
    char error[256];
    ctl->state = RUN_IDLE;
    if (!ctl->session->open && (ctl->agent->messages || session_open(ctl->session, tools_workspace()))) {
        SetStatus(ctl, "Session file unavailable. Check %s or start a new session.", tools_workspace()); return;
    }
    if (!ctl->agent->messages) ctl->host->load_workspace_instructions(ctl->context);
    if (agent_begin(ctl->agent, ctl->pending, error, sizeof(error))) { SetStatus(ctl, "%s", error); return; }
    view_image_reset();
    ShowMessage(ctl, "You", ctl->pending);
    ctl->host->clear_prompt(ctl->context);
    SetSendEnabled(ctl, 0);
    /* Remote MCP discovery runs before the first model request; everything
     * else continues into the request, natively when no tools were admitted. */
    if (mcp_run_start()) {
        ctl->state = RUN_MCP;
        SetStatus(ctl, "Discovering remote MCP tools...");
        return;
    }
    StartModelRequest(ctl);
}

static void StartContextLookup(RunCtl *ctl)
{
    char path[CHAT_MODEL_CAP * 3 + 32];
    int length;
    length = agent_model_query(path, sizeof(path), ctl->model);
    if (length < 0) { SendBegin(ctl); return; }
    length = http_build_get("openrouter.ai", path, ctl->net->request, sizeof(ctl->net->request));
    if (length < 0) { SendBegin(ctl); return; }
    /* Remember the attempt before the network: at most one lookup per model
     * per launch, whatever the outcome. */
    strcpy(ctl->model_info_model, ctl->model); memset(&ctl->model_info, 0, sizeof(ctl->model_info));
    ctl->model_info.context_length = -1; ctl->model_info_attempted = 1;
    run_ctl_open_transport(ctl);
    ctl->start_ticks = ctl->host->tick_count(ctl->context);
    if (ctl->host->network_start(ctl->context, ctl->net, ctl->net->request, (size_t)length) < 0) {
        run_ctl_close_context(ctl); SendBegin(ctl); return;
    }
    ctl->state = RUN_CONTEXT_LOOKUP;
    SetSendEnabled(ctl, 0);
    SetStatus(ctl, "Checking context window for %s...", ctl->model);
}

static void SendAdvance(RunCtl *ctl)
{
    if (!(ctl->model_info_attempted && !strcmp(ctl->model_info_model, ctl->model)) && ModelLookupAllowed(ctl->model)) {
        StartContextLookup(ctl); return;
    }
    SendBegin(ctl);
}

static void FinishContextLookup(RunCtl *ctl, int completed)
{
    AgentModelInfo info;
    if (completed && ctl->net->status == 200 && !agent_model_info(ctl->net->body, ctl->net->body_len, ctl->model, &info))
        ctl->model_info = info;
    ctl->host->model_info_changed(ctl->context);
    if (ctl->net->ctx || ctl->ot_open) run_ctl_close_context(ctl);
    {
        if (strcmp(ctl->prefs->model, ctl->model)) {
            ctl->state = RUN_IDLE; SetSendEnabled(ctl, 1); ctl->host->focus_prompt(ctl->context);
            SetStatus(ctl, "Model changed during the context lookup. Send again to continue.");
            return;
        }
    }
    ctl->state = RUN_IDLE;
    SendAdvance(ctl);
}

void run_ctl_send(RunCtl *ctl)
{
    static char prompt[CHAT_PROMPT_CAP]; /* static: the application stack is small */
    if (run_ctl_busy(ctl)) return;
    if (ctl->exchange_drain) { SetStatus(ctl, "The stopped request is still closing; send again in a moment."); return; }
    if (mcp_run_draining()) { SetStatus(ctl, "The stopped MCP connection is still closing; send again in a moment."); return; }
    if (!ctl->prefs->api_key[0]) { SetStatus(ctl, "No API key: choose Preferences from the Edit menu."); return; }
    strcpy(ctl->model, ctl->prefs->model);
    if (ctl->host->read_prompt(ctl->context, prompt, sizeof(prompt)) < 0) { SetStatus(ctl, "Message is too long."); return; }
    if (*ctl->model && !model_id_valid(ctl->model)) {
        SetStatus(ctl, "Use an OpenRouter model ID without spaces."); return;
    }
    if (!*ctl->model || prompt_is_blank(prompt)) { SetStatus(ctl, "Enter a model and message first."); return; }
    if (text_to_utf8(prompt, strlen(prompt), ctl->pending, sizeof(ctl->pending)) < 0) { SetStatus(ctl, "Could not convert message to UTF-8."); return; }
    SendAdvance(ctl);
}

static void StepContextLookup(RunCtl *ctl)
{
    int lookup;
    if (ctl->host->tick_count(ctl->context) - ctl->start_ticks > 30UL * 60UL) { FinishContextLookup(ctl, 0); return; }
    lookup = ctl->host->network_step(ctl->context, ctl->net);
    if (lookup < 0) { FinishContextLookup(ctl, 0); return; }
    if (lookup == 0) {
        MacTLS_State state = ctl->host->network_state(ctl->context, ctl->net);
        if (state == kMacTLS_Handshaking) SetStatus(ctl, "TLS handshake...");
        else if (state == kMacTLS_Connected) SetStatus(ctl, "Checking context window for %s...", ctl->model);
        return;
    }
    FinishContextLookup(ctl, 1);
}

static void StepPendingTool(RunCtl *ctl)
{
    const PendingState *tool = PendingStateForRun(ctl->state);
    char error[256];
    int result;
    int status = PendingToolStep(ctl, tool, 0, &result, error, sizeof(error));
    if (status == 2) return;
    if (status) { run_ctl_abort(ctl, error); return; }
    if (result) run_ctl_abort(ctl, tool->failure);
}

static void StepTools(RunCtl *ctl)
{
    char error[256];
    JsonToken tokens[128];
    int result;
    AgentCall *call;
    const ToolDef *def;
    const PendingState *pending;
    if (ctl->agent->next == ctl->agent->count) {
        /* Tool messages cannot carry pixels: an image from this round follows
         * the last tool result, as a user message for the next request. */
        if (ctl->host->note_project_instructions(ctl->context)) { run_ctl_abort(ctl, "Could not record project instructions. Stop and inspect the session."); return; }
        const AgentImage *shot = view_image_take();
        if (shot && agent_attach_image(ctl->agent, shot)) { run_ctl_abort(ctl, "Could not record the attached image. Stop and inspect the session."); return; }
        ctl->state = RUN_NEXT_REQUEST; return;
    }
    if (ctl->agent->tool_count >= ctl->prefs->max_tools) { PauseRunAtLimit(ctl); return; }
    call = &ctl->agent->calls[ctl->agent->next];
    ctl->tool_start = ctl->host->tick_count(ctl->context);
    SetStatus(ctl, "Running %s (%d/%d)...", call->name, ctl->agent->next + 1, ctl->agent->count);
    ToolEventsReset(ctl->tool_events, sizeof(ctl->tool_events));
    {
        char id[800], name[400], started[1300];
        if (json_quote(call->id, id, sizeof(id)) < 0 || json_quote(call->name, name, sizeof(name)) < 0) {
            run_ctl_abort(ctl, "Could not encode tool start."); return;
        }
        snprintf(started, sizeof(started), "{\"call_id\":%s,\"name\":%s}", id, name);
        if (session_journal(ctl->session, "tool_started", started)) { run_ctl_abort(ctl, "Could not record tool start; no tool executed."); return; }
        ToolEventsAppend(ctl->tool_events, sizeof(ctl->tool_events), "tool_started");
    }
    view_image_set_vision(ctl->model_info_attempted && !strcmp(ctl->model_info_model, ctl->model) ? ctl->model_info.vision : AGENT_VISION_UNKNOWN);
    def = tools_lookup(call->name);
    /* A frozen remote MCP name dispatches to its own run state before the
     * native preflight: that guard is registry-driven and remote tools are
     * not in the registry, so it would refuse them as unknown. The server
     * validates their arguments (ADR-0003 keeps them read-only). */
    if (!def && mcp_run_owns(call->name)) { StartMcpCall(ctl, call); return; }
    pending = def ? PendingStateForDef(def) : NULL;
    /* One registry-driven preflight before either dispatch path. Pending rows
     * inherit the guard even when their begin function belongs to a module. */
    if (tools_check_arguments(def, call, tokens, ctl->tool_result, sizeof(ctl->tool_result))) result = 0;
    else if (pending && pending->begin) {
        result = pending->begin(call, ctl->tool_result, sizeof(ctl->tool_result), run_ctl_tool_journal, ctl, ctl->host->tick_count(ctl->context));
    } else if (def && def->id == TOOL_read_build_log) {
        build_project_log(call, ctl->tool_result, sizeof(ctl->tool_result)); result = 0;
    } else result = tools_execute_checked(call, tokens, ctl->tool_result, sizeof(ctl->tool_result), run_ctl_tool_journal, ctl, ctl->host->tick_count(ctl->context));
    if (pending && result == 2) { ctl->state = pending->state; SetStatus(ctl, "%s", pending->status); return; }
    LogToolTiming(ctl, ctl->agent->next + 1, call->name);
    if (agent_tool_result(ctl->agent, ctl->tool_result, error, sizeof(error))) { run_ctl_abort(ctl, error); return; }
    ShowToolResult(ctl, call, call->name, ctl->tool_result);
    if (result) run_ctl_abort(ctl, "Mutation stopped. Inspect the result and session recovery records; do not retry automatically.");
}

/* Model and handoff share one exchange, but commit different results. */
static void StepModelExchange(RunCtl *ctl)
{
    char error[256];
    int result;
    if (ctl->host->tick_count(ctl->context) - ctl->start_ticks > 120UL * 60UL) { run_ctl_abort(ctl, "Request timed out. Completed results retained."); return; }
    result = ctl->host->network_step(ctl->context, ctl->net);
    ObserveRound(ctl);
    if (result < 0) { run_ctl_abort(ctl, ctl->net->error); return; }
    if (!result) {
        MacTLS_State state = ctl->host->network_state(ctl->context, ctl->net);
        if (state == kMacTLS_Handshaking) SetStatus(ctl, "TLS handshake...");
        else if (state == kMacTLS_Connected) SetStatus(ctl, "Waiting for model (%s, %lu bytes)...", TLSVersionLabel(ctl->net->version), (unsigned long)ctl->net->received);
        return;
    }
    if (ctl->state == RUN_HANDOFF) {
        ObserveCompletionTokens(ctl);
        if (agent_handoff_response(ctl->net->body, ctl->net->body_len, ctl->net->status, ctl->handoff_summary,
            sizeof(ctl->handoff_summary), error, sizeof(error))) { JournalModelError(ctl, error); run_ctl_abort(ctl, error); return; }
        FinishChatContext(ctl);
        /* The summary completion is provider-billed like any other round. */
        agent_usage_absorb(ctl->agent, ctl->net->body, ctl->net->body_len);
        run_ctl_log_round(ctl, "ok");
        if (session_commit_handoff(ctl->session, ctl->agent, ctl->handoff_summary, ctl->handoff_path, sizeof(ctl->handoff_path))) {
            run_ctl_abort(ctl, "Could not save/verify handoff and new journal. History was not cleared."); return;
        }
        ctl->state = RUN_IDLE; SetSendEnabled(ctl, 1); ctl->host->focus_prompt(ctl->context);
        ShowMessage(ctl, "Saved handoff", ctl->handoff_path);
        ShowMessage(ctl, "Handoff summary", ctl->handoff_summary);
        SetStatus(ctl, "Handoff saved; fresh history is ready. Send a message to continue.");
        return;
    }
    ObserveCompletionTokens(ctl);
    if (agent_response(ctl->agent, ctl->net->body, ctl->net->body_len, ctl->net->status, error, sizeof(error))) { JournalModelError(ctl, error); run_ctl_abort(ctl, error); return; }
    FinishChatContext(ctl);
    run_ctl_log_round(ctl, ctl->agent->truncated ? "truncated" : ctl->agent->discarded ? "discarded" : "ok");
    if (ctl->agent->discarded) {
        char note[160];
        ctl->state = RUN_NEXT_REQUEST;
        snprintf(note, sizeof(note), "Reply discarded: %s. Nothing in it ran. Asking the model to retry.", ctl->agent->discarded);
        ShowMessage(ctl, "Notice", note);
        SetStatus(ctl, "Reply discarded; asking the model to retry smaller.");
        snprintf(note, sizeof(note), "Reply discarded (%s); model told to retry.", ctl->agent->discarded);
        LogLine(ctl, note);
        return;
    }
    if (ctl->agent->truncated) {
        ctl->state = RUN_NEXT_REQUEST;
        ShowMessage(ctl, "Notice", "Reply cut off at the output token limit; nothing in it ran. Asking the model to retry.");
        SetStatus(ctl, "Output token limit reached; asking the model to retry.");
        LogLine(ctl, "Reply truncated at the output token limit; model told to retry.");
        return;
    }
    if (*ctl->agent->text) ShowMessage(ctl, "Sherclawk", ctl->agent->text);
    if (ctl->agent->count) {
        ctl->state = RUN_TOOLS; SetStatus(ctl, "Model requested %d tool(s).", ctl->agent->count);
    } else {
        mcp_run_end(); /* the run is over: wipe the client and its credentials */
        ctl->state = RUN_IDLE; SetSendEnabled(ctl, 1); ctl->host->focus_prompt(ctl->context);
        if (ctl->agent->limited) ShowMessage(ctl, "Notice", "Reply incomplete: output token limit reached.");
        if (ctl->agent->used >= AGENT_HISTORY_CAP * 3 / 4)
            SetStatus(ctl, "History nearly full. Save Handoff (Command-H) to free history.");
        else SetStatus(ctl, "Done - %d model rounds, %d tools. Session saved.", ctl->agent->rounds, ctl->agent->tool_count);
        LogLine(ctl, "Agent run completed.");
    }
}

void run_ctl_step(RunCtl *ctl)
{
    switch (ctl->state) {
    case RUN_IDLE: break;
    case RUN_CONTEXT_LOOKUP: StepContextLookup(ctl); break;
    case RUN_NEXT_REQUEST: StartModelRequest(ctl); break;
    case RUN_READ_TEXT:
    case RUN_EDIT_TEXT:
    case RUN_BUILD:
    case RUN_QUIT:
    case RUN_LAUNCH:
    case RUN_VIEW_IMAGE:
    case RUN_COPY_FILE: StepPendingTool(ctl); break;
    case RUN_MCP: StepMcp(ctl); break;
    case RUN_TOOLS: StepTools(ctl); break;
    case RUN_MODEL_REQUEST:
    case RUN_HANDOFF: StepModelExchange(ctl); break;
    }
}

/* Pump only until the connect settles. The independent 30-second backstop
 * survives aborts and logs any forced close; it never sends another request. */
void run_ctl_drain_exchange(RunCtl *ctl)
{
    int expired;
    if (!ctl->exchange_drain) return;
    expired = ctl->host->tick_count(ctl->context) - ctl->drain_start_ticks > 30UL * 60UL;
    if (ExchangeConnecting(ctl) && !expired) {
        ctl->host->network_pump(ctl->context, ctl->net);
        return;
    }
    if (expired && ExchangeConnecting(ctl))
        LogLine(ctl, "Abandoned exchange still connecting after 30 s; forcing close.");
    run_ctl_close_context(ctl);
    ctl->exchange_drain = 0;
}
