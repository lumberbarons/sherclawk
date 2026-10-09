/* JSON wire behavior and history are shared by guest UI and diagnostic rigs.
 * A failed response never changes conversation history or the transcript. */
#include "chat.h"
#include "json.h"
#include "text.h"
#include <stdio.h>
#include <string.h>

void chat_reset(Chat *c) { memset(c, 0, sizeof(*c)); }
static int append(char *out, size_t cap, size_t *at, const char *s)
{
    size_t n = strlen(s);
    if (*at >= cap || n >= cap - *at) return -1;
    memcpy(out + *at, s, n + 1); *at += n; return 0;
}
void chat_tool_forget(Chat *c) { c->tool_end = 0; }
void chat_tool_note(Chat *c, size_t start, const char *name)
{
    size_t end = strlen(c->transcript);
    c->tool_end = 0;
    if (strlen(name) >= sizeof(c->tool_name) || start >= end) return;
    strcpy(c->tool_name, name);
    c->tool_at = start; c->tool_end = end; c->tool_count = 1;
}
int chat_tool_collapse(Chat *c, const char *name)
{
    char utf8[96], line[96];
    size_t n;
    if (!c->tool_end || strcmp(c->tool_name, name) || strlen(c->transcript) != c->tool_end ||
        c->tool_count >= 99999) return 0;
    snprintf(utf8, sizeof(utf8), "\xE2\x80\xA2 %s x%d", name, c->tool_count + 1);
    if (text_to_macroman(utf8, line, sizeof(line)) < 0) return 0;
    n = strlen(line);
    if (n + 3 >= sizeof(c->transcript) - c->tool_at) return 0;
    memcpy(c->transcript + c->tool_at, line, n); memcpy(c->transcript + c->tool_at + n, "\r\r", 3);
    c->tool_count++; c->tool_end = c->tool_at + n + 2;
    return 1;
}
static int quoted(char *out, size_t cap, size_t *at, const char *s)
{
    int n = json_quote(s, out + *at, cap - *at);
    if (n < 0) return -1;
    *at += (size_t)n; return 0;
}
int chat_request(const Chat *c, const char *model, const char *prompt, char *out, size_t cap)
{
    size_t at = 0;
    int i;
    if (!*model || !*prompt || c->messages >= CHAT_MESSAGE_MAX || cap > CHAT_REQUEST_CAP) return -1;
    if (append(out, cap, &at, "{\"model\":") || quoted(out, cap, &at, model) ||
        append(out, cap, &at, ",\"stream\":false,\"max_tokens\":512,\"messages\":[")) return -1;
    for (i = 0; i <= c->messages; i++) {
        const char *s = i == c->messages ? prompt : c->history + c->offsets[i];
        if ((i && append(out, cap, &at, ",")) ||
            append(out, cap, &at, i % 2 ? "{\"role\":\"assistant\",\"content\":" :
                                       "{\"role\":\"user\",\"content\":") ||
            quoted(out, cap, &at, s) || append(out, cap, &at, "}")) return -1;
    }
    if (append(out, cap, &at, "]}")) return -1;
    return (int)at;
}
int chat_reply(const char *body, size_t len, int status, char *reply, size_t cap,
               int *limited, char *error, size_t error_cap)
{
    /* Static to avoid a 32 KB allocation on the classic app's stack. */
    static JsonToken t[2048];
    char reason[80], detail[256];
    int count, e, choices, choice, message, content, finish;
    *limited = 0; reply[0] = 0;
    snprintf(error, error_cap, "HTTP %d: invalid or unsupported JSON response.", status);
    count = json_parse(body, len, t, 2048);
    if (count < 1 || t[0].type != JSON_OBJECT) return -1;
    e = json_member(body, t, 0, "error");
    if (e >= 0 || status < 200 || status >= 300) {
        int m = json_member(body, t, e, "message");
        if (m < 0) m = json_member(body, t, 0, "message");
        if (json_string(body, t, m, detail, sizeof(detail)) >= 0)
            snprintf(error, error_cap, "HTTP %d: %s", status, detail);
        else snprintf(error, error_cap, "HTTP %d: OpenRouter request failed.", status);
        return -1;
    }
    choices = json_member(body, t, 0, "choices");
    if (choices < 0 || t[choices].type != JSON_ARRAY || t[choices].next == choices + 1) return -1;
    choice = choices + 1;
    message = json_member(body, t, choice, "message");
    content = json_member(body, t, message, "content");
    finish = json_member(body, t, choice, "finish_reason");
    if (json_string(body, t, finish, reason, sizeof(reason)) < 0) return -1;
    if (!strcmp(reason, "length")) *limited = 1;
    else if (strcmp(reason, "stop")) {
        snprintf(error, error_cap, "Unsupported completion finish reason: %s", reason); return -1;
    }
    if (json_string(body, t, content, reply, cap) < 0 || !*reply) {
        snprintf(error, error_cap, "The model returned no usable text, or the reply is too large."); return -1;
    }
    error[0] = 0; return 0;
}
int chat_commit(Chat *c, const char *prompt, const char *reply, int limited)
{
    static char display[CHAT_TRANSCRIPT_CAP];
    static char user[CHAT_TRANSCRIPT_CAP], assistant[CHAT_TRANSCRIPT_CAP];
    size_t at = 0, pn = strlen(prompt) + 1, rn = strlen(reply) + 1;
    int messages = c->messages;
    if (messages + 2 > CHAT_MESSAGE_MAX || pn > sizeof(c->history) - c->used ||
        rn > sizeof(c->history) - c->used - pn ||
        text_to_macroman(prompt, user, sizeof(user)) < 0 ||
        text_to_macroman(reply, assistant, sizeof(assistant)) < 0) return -1;
    if (append(display, sizeof(display), &at, c->transcript) ||
        append(display, sizeof(display), &at, "You:\r") ||
        append(display, sizeof(display), &at, user) ||
        append(display, sizeof(display), &at, "\r\rAssistant:\r") ||
        append(display, sizeof(display), &at, assistant) ||
        (limited && append(display, sizeof(display), &at, "\r[Reply incomplete: token limit reached.]")) ||
        append(display, sizeof(display), &at, "\r\r")) return -1;
    /* TextEdit coordinates are signed shorts as well as its byte count.
     * Keep enough vertical room for wrapped lines at the fixed window width. */
    {
        size_t i, lines = 0;
        for (i = 0; i < at; i++) if (display[i] == 13 && ++lines > 1500) return -1;
    }
    /* Every possible failure has been checked before mutating the history. */
    memcpy(c->transcript, display, at + 1);
    c->tool_end = 0;
    c->offsets[messages] = c->used;
    memcpy(c->history + c->used, prompt, pn); c->used += pn;
    c->offsets[messages + 1] = c->used;
    memcpy(c->history + c->used, reply, rn); c->used += rn;
    c->messages += 2; return 0;
}
