/* Typed model history and a bounded, sequential native tool conversation. */
#ifndef SHERCLAWK_AGENT_H
#define SHERCLAWK_AGENT_H
#include <stddef.h>
#define AGENT_HISTORY_CAP 262144
#define AGENT_RESULT_CAP 1536
#define AGENT_RESULT_WIRE_CAP (AGENT_RESULT_CAP * 6 + 256)
#define AGENT_CALL_MAX 4
#define AGENT_ARGUMENT_CAP 8192
#define AGENT_TEXT_CAP 16384
#define AGENT_TURN_MAX 16
#define AGENT_TOOL_MAX 32

typedef struct { char id[128], name[64], arguments[AGENT_ARGUMENT_CAP]; } AgentCall;
typedef int (*AgentJournal)(void *context, const char *event, const char *json);
typedef struct {
    char history[AGENT_HISTORY_CAP];
    char text[AGENT_TEXT_CAP];
    AgentCall calls[AGENT_CALL_MAX];
    size_t used;
    int messages, count, next, rounds, tool_count, active, limited;
    AgentJournal journal;
    void *journal_context;
} Agent;
void agent_reset(Agent *a, AgentJournal journal, void *context);
int agent_begin(Agent *a, const char *prompt, char *error, size_t cap);
int agent_request(const Agent *a, const char *model, char *out, size_t cap);
/* Independent, tool-free summary request works even when history is full.
 * Seed a fresh candidate only; caller publishes it after durable persistence. */
int agent_handoff_request(const Agent *a, const char *model, char *out, size_t cap);
int agent_handoff_response(const char *body, size_t len, int status, char *summary,
                           size_t cap, char *error, size_t error_cap);
int agent_handoff_seed(Agent *candidate, const char *summary, const char *source,
                       const char *path);
/* Complete responses are recorded before any tool may execute. */
int agent_response(Agent *a, const char *body, size_t len, int status, char *error, size_t cap);
int agent_tool_result(Agent *a, const char *result, char *error, size_t cap);
int agent_stop(Agent *a, const char *reason);
const char *agent_tool_schemas(void);
#endif
