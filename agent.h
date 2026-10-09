/* Typed model history and a bounded, sequential native tool conversation. */
#ifndef SHERCLAWK_AGENT_H
#define SHERCLAWK_AGENT_H
#include <stddef.h>
#include "chat.h"
#define AGENT_HISTORY_CAP 393216
#define AGENT_RESULT_CAP 1536
#define AGENT_RESULT_WIRE_CAP (AGENT_RESULT_CAP * 6 + 256)
#define AGENT_CALL_MAX 4
#define AGENT_ARGUMENT_CAP 8192
#define AGENT_TEXT_CAP 16384
/* Visible text of one model reply: AGENT_MAX_TOKENS of text at up to ~6 bytes
 * per token, kept separate from the user-prompt and handoff buffers. */
#define AGENT_REPLY_CAP 40960
#define AGENT_TURN_MAX 32
#define AGENT_TOOL_MAX 64
#define AGENT_MODEL_NAME_CAP 64
#define AGENT_EFFORT_CAP 16
#define AGENT_EFFORT_MAX 8
/* Rows shown or read from one catalog page by the model chooser. */
#define AGENT_MODEL_ROWS_MAX 10
/* Completion budget per agent request, reasoning included. */
#define AGENT_MAX_TOKENS 6000
#define AGENT_HANDOFF_MAX_TOKENS 1536
/* Display-grade cost accumulator clamp, in millionths of a US dollar. */
#define AGENT_COST_MICROS_MAX 9000000000000000LL

typedef struct { char id[128], name[64], arguments[AGENT_ARGUMENT_CAP]; } AgentCall;
/* One /api/v1/models?q= row: the catalog display name, context_length (-1
 * when the row has none) and the optional reasoning block, whose effort names
 * keep the provider's order. */
typedef struct {
    char name[AGENT_MODEL_NAME_CAP];
    long context_length;
    int reasoning, mandatory, default_enabled;
    char default_effort[AGENT_EFFORT_CAP];
    char supported_efforts[AGENT_EFFORT_MAX][AGENT_EFFORT_CAP];
    int effort_count;
} AgentModelInfo;
/* One display row of a /api/v1/models page: the exact id that gets saved and
 * the metadata of the row that carries it. */
typedef struct {
    char id[CHAT_MODEL_CAP];
    AgentModelInfo info;
} AgentModelRow;
typedef int (*AgentJournal)(void *context, const char *event, const char *json);
typedef struct {
    char history[AGENT_HISTORY_CAP];
    char text[AGENT_REPLY_CAP];
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
 * totals only for present, sane values. */
void agent_usage_absorb(Agent *a, const char *body, size_t len);
/* Select the data row whose id equals model exactly and copy its name,
 * context_length and reasoning metadata into info. Returns 0 when found, -1
 * for a malformed, over-cap or matching-free page; absent optional fields
 * keep their zero defaults. */
int agent_model_info(const char *body, size_t len, const char *model, AgentModelInfo *info);
/* Read up to max rows of one /api/v1/models page in page order, skipping rows
 * without a usable id. Returns the row count, 0 for a malformed or empty page. */
int agent_model_page(const char *body, size_t len, AgentModelRow *rows, int max);
/* Case-insensitive substring match against a row's id or name for local
 * filtering; empty text matches every row. */
int agent_model_row_match(const AgentModelRow *row, const char *text);
/* Index of the row whose id equals model exactly, or -1. */
int agent_model_row_find(const AgentModelRow *rows, int count, const char *model);
/* Path of the popular catalog page the Preferences dialog loads first:
 * /api/v1/models?limit=10&sort=most-popular. Returns the length, or -1. */
int agent_popular_query(char *out, size_t cap);
/* Build the lookup path /api/v1/models?q=<model>&limit=10, percent-encoding
 * model bytes outside [A-Za-z0-9-._~]. Returns the path length, or -1 when
 * it does not fit. */
int agent_model_query(char *out, size_t cap, const char *model);
/* usage.completion_tokens, and reasoning_tokens (-1 when not reported). Returns
 * 0 when the completion count is present, -1 otherwise. */
int agent_usage_completion(const char *body, size_t len, long *completion, long *reasoning);
const char *agent_tool_schemas(void);
#endif
