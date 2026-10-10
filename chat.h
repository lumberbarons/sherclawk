/* Bounded in-memory conversation. Pending turns are committed atomically. */
#ifndef SHERCLAWK_CHAT_H
#define SHERCLAWK_CHAT_H
#include <stddef.h>
#define CHAT_REQUEST_CAP 1081344
#define CHAT_RESPONSE_CAP 65536
#define CHAT_TRANSCRIPT_CAP 30001
#define CHAT_PROMPT_CAP 8193
#define CHAT_MODEL_CAP 256
#define CHAT_HISTORY_CAP 32768
#define CHAT_MESSAGE_MAX 20
typedef struct {
    char history[CHAT_HISTORY_CAP];
    size_t offsets[CHAT_MESSAGE_MAX], used;
    int messages;
    char transcript[CHAT_TRANSCRIPT_CAP];
    /* The last transcript entry, when it is a tool call line that a following
     * call to the same tool may fold into: where it starts, where the
     * transcript ended after it (0 when there is none), its tool and run. */
    size_t tool_at, tool_end;
    int tool_count;
    char tool_name[64];
} Chat;
void chat_reset(Chat *chat);
/* Returns the entry offset, or -1 if its label cannot fit. Older display text
 * may be replaced by a session-file notice; history is never changed. */
long chat_append_message(Chat *chat, const char *label, const char *text);
int chat_request(const Chat *chat, const char *model, const char *prompt,
                 char *out, size_t cap);
/* Returns 0 reply, -1 API/protocol error; error is safe human-readable text. */
int chat_reply(const char *body, size_t len, int status, char *reply, size_t cap,
               int *limited, char *error, size_t error_cap);
/* Collapsible tool lines. chat_tool_note marks the entry that starts at start
 * (the transcript's newest) as one call of tool name; chat_tool_forget drops
 * the mark. chat_tool_collapse returns 1 after rewriting that entry as
 * "<bullet> name xN" when the transcript still ends with it and the next call is
 * to the same tool, and 0 (nothing changed) when the caller must append. */
void chat_tool_note(Chat *chat, size_t start, const char *name);
void chat_tool_forget(Chat *chat);
int chat_tool_collapse(Chat *chat, const char *name);
int chat_commit(Chat *chat, const char *prompt, const char *reply, int limited);
#endif
