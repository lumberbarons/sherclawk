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
#define AGENT_TURN_MAX 32
#define AGENT_TOOL_MAX 64
/* Completion budget per agent request, reasoning included. */
#define AGENT_MAX_TOKENS 3072
#define AGENT_HANDOFF_MAX_TOKENS 1536
/* Display-grade cost accumulator clamp, in millionths of a US dollar. */
#define AGENT_COST_MICROS_MAX 9000000000000000LL

typedef struct { char id[128], name[64], arguments[AGENT_ARGUMENT_CAP]; } AgentCall;
typedef int (*AgentJournal)(void *context, const char *event, const char *json);
typedef struct {
    char history[AGENT_HISTORY_CAP];
    char text[AGENT_TEXT_CAP];
    AgentCall calls[AGENT_CALL_MAX];
    size_t used;
    int messages, count, next, rounds, tool_count, active, limited, truncated;
    long long cost_micros;
    long context_tokens;
    int cost_seen, context_seen;
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
/* Complete responses are recorded before any tool may execute. A reply cut off
 * at the output limit with no usable text, or with tool calls, executes
 * nothing: it is discarded, a user notice telling the model to retry smaller
 * is recorded instead, and the run stays active with `truncated` set and no
 * pending calls (the caller sends the next request). Text-only cut-offs end the
 * run with `limited` set. */
int agent_response(Agent *a, const char *body, size_t len, int status, char *error, size_t cap);
int agent_tool_result(Agent *a, const char *result, char *error, size_t cap);
int agent_stop(Agent *a, const char *reason);
/* Provider-reported usage is display-grade: absorb never fails and moves
 * totals only for present, sane values. context_limit returns the largest
 * data.endpoints[].context_length, or -1. */
void agent_usage_absorb(Agent *a, const char *body, size_t len);
long agent_context_limit(const char *body, size_t len);
/* usage.completion_tokens, and reasoning_tokens (-1 when not reported). Returns
 * 0 when the completion count is present, -1 otherwise. */
int agent_usage_completion(const char *body, size_t len, long *completion, long *reasoning);
const char *agent_tool_schemas(void);
#endif
