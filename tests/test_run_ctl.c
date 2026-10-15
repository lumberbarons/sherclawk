/* Characterize the production run transitions with the real agent protocol.
 * The network, durable journal sink and native effects are controlled fakes;
 * session.c's File Manager durability/fault tests remain in test_session.c. */
#include "app/run_ctl.h"
#include "tools/tools.h"
#include "tools/view_image.h"
#include "native/build_project.h"
#include "native/run_application.h"
#include "mcp/run.h"
#include "agent/json.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static RunCtl ctl;
static Agent agent;
static Session session;
static Prefs prefs;
static ChatNetwork net;
static struct {
    uint32_t tick;
    MacTLS_State state;
    char prompt[CHAT_PROMPT_CAP], status[256];
    char events[64][40], records[64][4096];
    int count, starts, steps, pumps, closes, opens, transport_closes, yields;
    int enabled, cleared, focused, messages, tool_messages, executed, pending_steps;
    int stopped, commit, network_result, start_failed, pending_result;
    const char *reply, *fail_event;
    int http_status, discovery, remote_pending, remote_stops, mcp_drain, mcp_end;
    McpRunPhase phase;
} fake;

int session_open(Session *s, const char *workspace)
{
    (void)workspace; s->open = 1; return 0;
}
void session_close(Session *s) { s->open = 0; }
int session_journal(void *context, const char *event, const char *json)
{
    JsonToken tokens[1024];
    assert(context == &session);
    assert(json_parse(json, strlen(json), tokens, 1024) > 0);
    if (fake.fail_event && !strcmp(fake.fail_event, event)) return -1;
    assert(fake.count < 64);
    snprintf(fake.events[fake.count], sizeof(fake.events[0]), "%s", event);
    snprintf(fake.records[fake.count], sizeof(fake.records[0]), "%s", json);
    fake.count++;
    return 0;
}
int session_commit_handoff(Session *s, Agent *a, const char *summary, char *path, size_t cap)
{
    assert(s == &session && a == &agent && strstr(summary, "Next"));
    snprintf(path, cap, "Retro68:Sherclawk Sessions:handoff.md");
    fake.commit++; return 0;
}
const char *tools_workspace(void) { return "Retro68:"; }
int tools_check_arguments(const ToolDef *def, const AgentCall *call, JsonToken *tokens, char *out, size_t cap)
{
    (void)def; (void)out; (void)cap;
    assert(json_parse(call->arguments, strlen(call->arguments), tokens, 128) > 0);
    return 0;
}
int tools_execute_checked(const AgentCall *call, const JsonToken *tokens, char *out, size_t cap,
                          AgentJournal journal, void *context, uint32_t tick)
{
    (void)tokens; (void)tick;
    assert(!strcmp(fake.events[fake.count - 1], "tool_started"));
    assert(journal == run_ctl_tool_journal && context == &ctl);
    fake.executed++;
    if (!strcmp(call->name, "read_text")) return 2;
    snprintf(out, cap, "{\"status\":\"ok\",\"native\":true}");
    return 0;
}
static int pending_step(char *out, size_t cap, uint32_t tick, int stop)
{
    (void)tick; fake.pending_steps++;
    if (stop) { fake.stopped++; snprintf(out, cap, "{\"status\":\"stopped\"}"); return 1; }
    snprintf(out, cap, "{\"status\":\"ok\",\"text\":\"retained\"}");
    return fake.pending_result;
}
#define STEP_STUB(name) int name(char *out, size_t cap, uint32_t tick, int stop) { return pending_step(out, cap, tick, stop); }
STEP_STUB(read_text_step)
STEP_STUB(edit_text_step)
STEP_STUB(copy_file_step)
STEP_STUB(build_project_step)
STEP_STUB(run_application_step)
STEP_STUB(quit_application_step)
STEP_STUB(view_image_step)
#define BEGIN_STUB(name) int name(const AgentCall *call, char *out, size_t cap, AgentJournal journal, void *context, uint32_t tick) { \
    (void)call; (void)out; (void)cap; (void)journal; (void)context; (void)tick; fake.executed++; return 2; }
BEGIN_STUB(build_project_begin)
BEGIN_STUB(run_application_begin)
BEGIN_STUB(quit_application_begin)
BEGIN_STUB(view_image_begin)
void build_project_log(const AgentCall *call, char *out, size_t cap)
{
    (void)call; snprintf(out, cap, "{}");
}
void view_image_reset(void) {}
void view_image_set_vision(int vision) { (void)vision; }
const AgentImage *view_image_held(void) { return NULL; }
const AgentImage *view_image_take(void) { return NULL; }
int mcp_run_start(void) { fake.phase = fake.discovery ? MCP_RUN_DISCOVERING : MCP_RUN_IDLE; return fake.discovery; }
McpRunPhase mcp_run_phase(void) { return fake.phase; }
McpDiscovery mcp_run_discover_step(void) { fake.phase = MCP_RUN_IDLE; return MCP_DISCOVERY_NATIVE; }
int mcp_run_admitted(void) { return 0; }
const char *mcp_run_alert(void) { return ""; }
int mcp_run_owns(const char *name) { return !strcmp(name, "mcp_remote"); }
int mcp_run_call(const AgentCall *call, char *out, size_t cap)
{
    (void)call; (void)out; (void)cap; fake.phase = MCP_RUN_CALLING; return 1;
}
int mcp_run_call_step(char *out, size_t cap)
{
    if (fake.remote_pending) return 0;
    snprintf(out, cap, "{\"remote\":true}"); fake.phase = MCP_RUN_IDLE; return 1;
}
int mcp_run_stop(char *out, size_t cap)
{
    if (fake.phase != MCP_RUN_CALLING) return 0;
    fake.remote_stops++; snprintf(out, cap, "{\"remote\":\"stopped\"}"); return 1;
}
void mcp_run_end(void) { fake.phase = MCP_RUN_IDLE; fake.mcp_end++; }
int mcp_run_draining(void) { return fake.mcp_drain; }
int mcp_run_drain_pending(void) { return fake.mcp_drain; }
void mcp_run_drain_step(void) { fake.mcp_drain = 0; }

static void status(void *context, const char *text) { (void)context; snprintf(fake.status, sizeof(fake.status), "%s", text); }
static void enabled(void *context, int on) { (void)context; fake.enabled = on; }
static void message(void *context, const char *label, const char *text)
{
    (void)context; (void)label; assert(text); fake.messages++;
}
static void tool_message(void *context, const AgentCall *call, const char *label, const char *text)
{
    (void)context; assert(call && label && *text); fake.tool_messages++;
}
static void log_line(void *context, const char *text) { (void)context; assert(*text); }
static uint32_t now(void *context) { (void)context; return fake.tick; }
static int read_prompt(void *context, char *out, size_t cap)
{
    (void)context; snprintf(out, cap, "%s", fake.prompt); return 0;
}
static void clear(void *context) { (void)context; fake.prompt[0] = 0; fake.cleared++; }
static void focus(void *context) { (void)context; fake.focused++; }
static void no_effect(void *context) { (void)context; }
static int note_project(void *context) { (void)context; return 0; }
static int start(void *context, ChatNetwork *n, const char *request, size_t len)
{
    (void)context;
    assert(n == &net && len && request == n->request);
    fake.starts++; n->ctx = (MacTLS_Context *)&fake;
    n->request_len = len; n->sent = n->received = n->body_len = 0; n->result = 0;
    return fake.start_failed ? -1 : 0;
}
static int step(void *context, ChatNetwork *n)
{
    (void)context; fake.steps++;
    if (fake.network_result == 1) {
        assert(fake.reply);
        snprintf(n->body, sizeof(n->body), "%s", fake.reply);
        n->body_len = strlen(n->body); n->status = fake.http_status;
        n->received = n->body_len; n->sent = n->request_len;
    }
    n->result = fake.network_result;
    return n->result;
}
static MacTLS_State state(void *context, ChatNetwork *n) { (void)context; (void)n; return fake.state; }
static void pump(void *context, ChatNetwork *n) { (void)context; (void)n; fake.pumps++; }
static void close_ctx(void *context, ChatNetwork *n) { (void)context; n->ctx = NULL; fake.closes++; }
static void open_transport(void *context) { (void)context; fake.opens++; }
static void close_transport(void *context) { (void)context; fake.transport_closes++; }
static void yield_transport(void *context) { (void)context; fake.yields++; }
static const RunHost host = {
    status, enabled, message, tool_message, log_line, now, read_prompt, clear,
    focus, no_effect, no_effect, note_project, start, step, state, pump, close_ctx,
    open_transport, close_transport, yield_transport
};
static const char done[] = "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"Done\"}}]}";
static const char calls[] = "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":["
    "{\"id\":\"one\",\"type\":\"function\",\"function\":{\"name\":\"get_environment\",\"arguments\":\"{}\"}},"
    "{\"id\":\"two\",\"type\":\"function\",\"function\":{\"name\":\"read_text\",\"arguments\":\"{\\\"path\\\":\\\"main.c\\\"}\"}}]}}]}";
static void setup(void)
{
    memset(&fake, 0, sizeof(fake)); memset(&session, 0, sizeof(session)); memset(&net, 0, sizeof(net));
    prefs_defaults(&prefs); strcpy(prefs.model, "test/model"); strcpy(prefs.api_key, "fake-key");
    prefs.max_rounds = 10; prefs.max_tools = 10;
    fake.state = kMacTLS_Connected; fake.http_status = 200; fake.enabled = 1;
    strcpy(fake.prompt, "Inspect files");
    run_ctl_init(&ctl, &host, NULL, &agent, &session, &prefs, &net);
    /* The confirmed row suppresses the optional catalog lookup. */
    strcpy(ctl.model_info_model, prefs.model); ctl.model_info_attempted = 1;
    agent_reset(&agent, session_journal, &session);
}
static void respond(const char *reply)
{
    fake.reply = reply; fake.network_result = 1; run_ctl_step(&ctl); fake.network_result = 0;
}
static void tools_ready(void)
{
    run_ctl_send(&ctl); respond(calls);
    assert(ctl.state == RUN_TOOLS && agent.count == 2 && fake.executed == 0);
}
static void test_send_tools_next_idle(void)
{
    setup(); run_ctl_send(&ctl);
    assert(ctl.state == RUN_MODEL_REQUEST && fake.starts == 1 && fake.count == 1);
    assert(!strcmp(fake.events[0], "user") && fake.cleared == 1 && !fake.enabled);
    assert(strstr(net.request, "test/model") && strstr(net.request, "Inspect files"));
    respond(calls);
    assert(ctl.state == RUN_TOOLS && fake.executed == 0 && fake.closes == 1);
    run_ctl_step(&ctl);
    assert(agent.next == 1 && fake.executed == 1 && fake.tool_messages == 1);
    assert(!strcmp(fake.events[2], "tool_started") && !strcmp(fake.events[3], "tool"));
    run_ctl_step(&ctl);
    assert(ctl.state == RUN_READ_TEXT && fake.executed == 2 && agent.next == 1);
    fake.pending_result = 2; run_ctl_step(&ctl);
    assert(ctl.state == RUN_READ_TEXT && agent.next == 1);
    fake.pending_result = 0; run_ctl_step(&ctl);
    assert(ctl.state == RUN_TOOLS && agent.next == 2 && strstr(agent.history, "retained"));
    run_ctl_step(&ctl);
    assert(ctl.state == RUN_NEXT_REQUEST && fake.starts == 1);
    run_ctl_step(&ctl);
    assert(ctl.state == RUN_MODEL_REQUEST && fake.starts == 2 && strstr(net.request, "retained"));
    respond(done);
    assert(!run_ctl_busy(&ctl) && !agent.active && agent.rounds == 2 && agent.tool_count == 2);
    assert(fake.enabled && fake.focused == 1 && strstr(fake.status, "Session saved"));
    assert(!strcmp(fake.events[fake.count - 1], "assistant"));
}
static void test_stop_retains_results_and_fills_unstarted(void)
{
    setup(); tools_ready(); run_ctl_step(&ctl);
    run_ctl_abort(&ctl, "User Stop");
    assert(!run_ctl_busy(&ctl) && !agent.active && agent.next == 2 && fake.executed == 1);
    assert(strstr(agent.history, "native") && strstr(agent.history, "User Stop"));
    assert(fake.enabled && fake.focused == 1 && !strcmp(fake.status, "User Stop"));
    setup(); tools_ready(); run_ctl_step(&ctl); run_ctl_step(&ctl);
    run_ctl_abort(&ctl, "Stop pending");
    assert(fake.stopped == 1 && fake.tool_messages == 2 && !run_ctl_busy(&ctl));
    assert(strstr(agent.history, "stopped") && agent.next == 2);
    run_ctl_step(&ctl); assert(fake.stopped == 1);
}
static void test_limits_pause_before_further_work(void)
{
    setup(); prefs.max_rounds = 1; tools_ready();
    run_ctl_step(&ctl); run_ctl_step(&ctl); run_ctl_step(&ctl); run_ctl_step(&ctl);
    assert(ctl.state == RUN_NEXT_REQUEST); run_ctl_step(&ctl);
    assert(!run_ctl_busy(&ctl) && fake.starts == 1 && strstr(fake.status, "round"));
    setup(); prefs.max_tools = 1; tools_ready(); run_ctl_step(&ctl); run_ctl_step(&ctl);
    assert(!run_ctl_busy(&ctl) && fake.executed == 1 && strstr(fake.status, "tool"));
}
static void test_model_error_and_journal_barrier(void)
{
    int i, found = 0;
    setup(); run_ctl_send(&ctl); fake.http_status = 401;
    respond("{\"error\":{\"message\":\"Invalid key\"}}");
    assert(!run_ctl_busy(&ctl) && strstr(fake.status, "Invalid key"));
    for (i = 0; i < fake.count; i++) if (!strcmp(fake.events[i], "model_error")) {
        assert(strstr(fake.records[i], "\"http_status\":401") && strstr(fake.records[i], "received_bytes") && strstr(fake.records[i], "Invalid key")); found++;
    }
    assert(found == 1);
    setup(); tools_ready(); fake.fail_event = "tool_started"; run_ctl_step(&ctl);
    assert(!run_ctl_busy(&ctl) && fake.executed == 0 && strstr(fake.status, "no tool executed"));
    setup(); fake.fail_event = "user"; run_ctl_send(&ctl);
    assert(!run_ctl_busy(&ctl) && fake.starts == 0 && !fake.cleared);
}
static void test_connect_stop_drains_without_sending(void)
{
    setup(); fake.state = kMacTLS_Connecting; run_ctl_send(&ctl);
    run_ctl_abort(&ctl, "Stop connecting");
    assert(!run_ctl_busy(&ctl) && ctl.exchange_drain && net.ctx && fake.closes == 0);
    strcpy(fake.prompt, "Again"); run_ctl_send(&ctl); assert(fake.starts == 1);
    run_ctl_drain_exchange(&ctl);
    assert(fake.pumps == 1 && fake.steps == 0 && fake.closes == 0);
    fake.state = kMacTLS_Connected; run_ctl_drain_exchange(&ctl);
    assert(!ctl.exchange_drain && !net.ctx && !ctl.ot_open && fake.transport_closes == 1 && fake.yields == 2);
    run_ctl_send(&ctl); assert(fake.starts == 2);
    setup(); fake.tick = UINT32_MAX - 100; fake.state = kMacTLS_Handshaking;
    run_ctl_send(&ctl); run_ctl_abort(&ctl, "Stop"); fake.tick += 30 * 60 + 1;
    run_ctl_drain_exchange(&ctl);
    assert(!ctl.exchange_drain && fake.closes == 1 && fake.pumps == 0);
}
static void test_lookup_stop_keeps_prompt_and_timeout_continues(void)
{
    setup(); ctl.model_info_attempted = 0; run_ctl_send(&ctl);
    assert(ctl.state == RUN_CONTEXT_LOOKUP && !fake.count && !fake.cleared);
    run_ctl_abort(&ctl, "Stop lookup");
    assert(!run_ctl_busy(&ctl) && !fake.count && !fake.cleared && !session.open);
    setup(); ctl.model_info_attempted = 0; run_ctl_send(&ctl);
    fake.tick += 30 * 60 + 1; run_ctl_step(&ctl);
    assert(ctl.state == RUN_MODEL_REQUEST && fake.starts == 2 && fake.cleared == 1);
    fake.tick += 120 * 60 + 1; run_ctl_step(&ctl);
    assert(!run_ctl_busy(&ctl) && strstr(fake.status, "timed out"));
}
static void test_handoff_stop_retains_history_and_success_commits(void)
{
    size_t used;
    setup(); run_ctl_send(&ctl); respond(done); used = agent.used;
    run_ctl_handoff(&ctl); assert(ctl.state == RUN_HANDOFF && fake.starts == 2);
    run_ctl_abort(&ctl, "Stop summary");
    assert(!run_ctl_busy(&ctl) && agent.used == used && session.open && !fake.commit);
    run_ctl_handoff(&ctl);
    respond("{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"Next: inspect sources\"}}]}");
    assert(!run_ctl_busy(&ctl) && fake.commit == 1 && *ctl.handoff_path && fake.enabled);
}
static void test_mcp_discovery_stop_and_deferred_transport_close(void)
{
    setup(); fake.discovery = 1; run_ctl_send(&ctl);
    assert(ctl.state == RUN_MCP && fake.starts == 0);
    run_ctl_step(&ctl); assert(ctl.state == RUN_MODEL_REQUEST && fake.starts == 1);
    respond("{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"remote\",\"type\":\"function\",\"function\":{\"name\":\"mcp_remote\",\"arguments\":\"{}\"}}]}}]}");
    run_ctl_step(&ctl); assert(ctl.state == RUN_MCP && fake.phase == MCP_RUN_CALLING);
    run_ctl_abort(&ctl, "Stop remote");
    assert(!run_ctl_busy(&ctl) && fake.remote_stops == 1 && strstr(agent.history, "stopped"));
    setup(); run_ctl_open_transport(&ctl); fake.mcp_drain = 1;
    run_ctl_close_context(&ctl);
    assert(ctl.ot_open && ctl.ot_close_pending && !fake.transport_closes);
    run_ctl_drain_mcp(&ctl);
    assert(!ctl.ot_open && !ctl.ot_close_pending && fake.transport_closes == 1);
}
static void test_lookup_completion_and_model_change(void)
{
    static const char catalog[] = "{\"data\":[{\"id\":\"test/model\",\"context_length\":64000}]}";
    setup(); ctl.model_info_attempted = 0; run_ctl_send(&ctl); respond(catalog);
    assert(ctl.state == RUN_MODEL_REQUEST && ctl.model_info.context_length == 64000);
    assert(fake.starts == 2 && fake.cleared == 1);
    setup(); ctl.model_info_attempted = 0; run_ctl_send(&ctl);
    strcpy(prefs.model, "other/model"); respond(catalog);
    assert(!run_ctl_busy(&ctl) && fake.starts == 1 && !fake.cleared && !fake.count);
    assert(fake.enabled && strstr(fake.status, "Model changed"));
}
static void test_network_failure_and_pending_recording_failure(void)
{
    setup(); fake.start_failed = 1; strcpy(net.error, "Allocation failed"); run_ctl_send(&ctl);
    assert(!run_ctl_busy(&ctl) && !agent.active && fake.starts == 1 && !net.ctx);
    assert(strstr(fake.status, "Allocation failed"));
    setup(); run_ctl_send(&ctl); strcpy(net.error, "Connection reset"); fake.network_result = -1;
    run_ctl_step(&ctl);
    assert(!run_ctl_busy(&ctl) && strstr(fake.status, "Connection reset") && fake.enabled);
    setup(); tools_ready(); run_ctl_step(&ctl); run_ctl_step(&ctl);
    fake.fail_event = "tool"; run_ctl_step(&ctl);
    assert(!run_ctl_busy(&ctl) && !session.open && fake.pending_steps == 1 && !fake.stopped);
}
static void test_remote_completion_and_preferences_drain(void)
{
    setup(); run_ctl_send(&ctl);
    respond("{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"remote\",\"type\":\"function\",\"function\":{\"name\":\"mcp_remote\",\"arguments\":\"{}\"}}]}}]}");
    run_ctl_step(&ctl); fake.remote_pending = 1; run_ctl_step(&ctl);
    assert(ctl.state == RUN_MCP && !agent.next);
    fake.remote_pending = 0; run_ctl_step(&ctl);
    assert(ctl.state == RUN_TOOLS && agent.next == 1 && fake.tool_messages == 1);
    assert(strstr(agent.history, "remote") && !fake.remote_stops);
    setup(); run_ctl_open_transport(&ctl); net.ctx = (MacTLS_Context *)&fake;
    fake.state = kMacTLS_Connecting; fake.tick = 100; run_ctl_begin_drain(&ctl);
    fake.tick = 200; run_ctl_begin_drain(&ctl);
    assert(ctl.drain_start_ticks == 100 && ctl.state == RUN_IDLE);
    run_ctl_drain_exchange(&ctl); assert(fake.pumps == 1 && fake.steps == 0);
    fake.state = kMacTLS_Connected; run_ctl_drain_exchange(&ctl);
    assert(!ctl.exchange_drain && !ctl.ot_open && !fake.count);
}
int main(void)
{
    test_send_tools_next_idle(); test_stop_retains_results_and_fills_unstarted();
    test_limits_pause_before_further_work(); test_model_error_and_journal_barrier();
    test_connect_stop_drains_without_sending(); test_lookup_stop_keeps_prompt_and_timeout_continues();
    test_handoff_stop_retains_history_and_success_commits(); test_mcp_discovery_stop_and_deferred_transport_close();
    test_lookup_completion_and_model_change(); test_network_failure_and_pending_recording_failure();
    test_remote_completion_and_preferences_drain();
    puts("run controller checks passed"); return 0;
}
