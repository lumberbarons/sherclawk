/* Cooperative send/tool/handoff controller. The event loop calls one step per
 * turn; RunHost supplies Toolbox/network effects and the wrapping 60 Hz clock. */
#ifndef SHERCLAWK_RUN_CTL_H
#define SHERCLAWK_RUN_CTL_H
#include "agent/network.h"
#include "app/preferences.h"
#include "app/session.h"
#include "app/timing.h"

typedef enum {
    RUN_IDLE, RUN_MODEL_REQUEST, RUN_TOOLS, RUN_NEXT_REQUEST,
    RUN_READ_TEXT, RUN_EDIT_TEXT, RUN_BUILD, RUN_QUIT, RUN_LAUNCH,
    RUN_VIEW_IMAGE, RUN_COPY_FILE, RUN_HANDOFF, RUN_CONTEXT_LOOKUP, RUN_MCP
} RunState;

typedef struct {
    void (*set_status)(void *, const char *);
    void (*set_send_enabled)(void *, int);
    void (*show_message)(void *, const char *, const char *);
    void (*show_tool_result)(void *, const AgentCall *, const char *, const char *);
    void (*log_line)(void *, const char *);
    uint32_t (*tick_count)(void *);
    int (*read_prompt)(void *, char *, size_t);
    void (*clear_prompt)(void *);
    void (*focus_prompt)(void *);
    void (*model_info_changed)(void *);
    void (*load_workspace_instructions)(void *);
    int (*note_project_instructions)(void *);
    int (*network_start)(void *, ChatNetwork *, const char *, size_t);
    int (*network_step)(void *, ChatNetwork *);
    MacTLS_State (*network_state)(void *, ChatNetwork *);
    void (*network_pump)(void *, ChatNetwork *);
    void (*close_ctx)(void *, ChatNetwork *);
    void (*open_transport)(void *);
    void (*close_transport)(void *);
    void (*yield_transport)(void *);
} RunHost;

typedef struct {
    const RunHost *host;
    void *context;
    Agent *agent;
    Session *session;
    Prefs *prefs;
    ChatNetwork *net;
    RunState state;
    int ot_open, ot_close_pending, exchange_drain;
    uint32_t start_ticks, drain_start_ticks, tool_start;
    RoundTiming round_timing;
    char model[CHAT_MODEL_CAP];
    char model_info_model[CHAT_MODEL_CAP];
    AgentModelInfo model_info;
    int model_info_attempted;
    char pending[CHAT_PROMPT_CAP * 3];
    char json[CHAT_REQUEST_CAP];
    char tool_result[AGENT_RESULT_CAP];
    char tool_events[192];
    char handoff_summary[8192], handoff_path[256];
} RunCtl;

/* Initialize once per application lifetime, before loading the saved model
 * row. New Chat resets the agent/session, never transport or process ownership. */
void run_ctl_init(RunCtl *, const RunHost *, void *, Agent *, Session *, Prefs *, ChatNetwork *);
int run_ctl_busy(const RunCtl *);
void run_ctl_send(RunCtl *);
void run_ctl_handoff(RunCtl *);
void run_ctl_abort(RunCtl *, const char *reason);
void run_ctl_step(RunCtl *);
/* Transfer a canceled Preferences fetch to the event loop without stepping it. */
void run_ctl_begin_drain(RunCtl *);
void run_ctl_drain_exchange(RunCtl *);
void run_ctl_drain_mcp(RunCtl *);
/* Shared with the modal Preferences lookup and MCP. These retain the existing
 * OT cycle/yields, including deferred teardown behind an MCP connect drain. */
void run_ctl_open_transport(RunCtl *);
void run_ctl_close_context(RunCtl *);
void run_ctl_abandon_context(RunCtl *);
void run_ctl_log_round(RunCtl *, const char *outcome);
/* AgentJournal context is RunCtl*, including for native/MCP tool events. */
int run_ctl_tool_journal(void *, const char *event, const char *json);
#endif
