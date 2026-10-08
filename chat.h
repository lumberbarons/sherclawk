/* Bounded in-memory conversation. Pending turns are committed atomically. */
#ifndef SHERCLAWK_CHAT_H
#define SHERCLAWK_CHAT_H
#include <stddef.h>
#define CHAT_REQUEST_CAP 425984
#define CHAT_RESPONSE_CAP 65536
#define CHAT_TRANSCRIPT_CAP 30001
#define CHAT_PROMPT_CAP 2049
#define CHAT_MODEL_CAP 256
#define CHAT_HISTORY_CAP 32768
#define CHAT_MESSAGE_MAX 20
typedef struct {
    char history[CHAT_HISTORY_CAP];
    size_t offsets[CHAT_MESSAGE_MAX], used;
    int messages;
    char transcript[CHAT_TRANSCRIPT_CAP];
} Chat;
void chat_reset(Chat *chat);
int chat_request(const Chat *chat, const char *model, const char *prompt,
                 char *out, size_t cap);
/* Returns 0 reply, -1 API/protocol error; error is safe human-readable text. */
int chat_reply(const char *body, size_t len, int status, char *reply, size_t cap,
               int *limited, char *error, size_t error_cap);
int chat_commit(Chat *chat, const char *prompt, const char *reply, int limited);
#endif
