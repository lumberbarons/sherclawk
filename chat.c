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
    size_t at = 0, pn = strlen(prompt) + 1, rn = strlen(reply) + 1, old = strlen(c->transcript);
    int messages = c->messages;
    if (messages + 2 > CHAT_MESSAGE_MAX || c->entries + 2 > CHAT_ENTRY_MAX || pn > sizeof(c->history) - c->used ||
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
    c->entry_at[c->entries++] = old;
    c->entry_at[c->entries++] = old + 5 + strlen(user) + 2;
    c->tool_end = 0;
    c->offsets[messages] = c->used;
    memcpy(c->history + c->used, prompt, pn); c->used += pn;
    c->offsets[messages + 1] = c->used;
    memcpy(c->history + c->used, reply, rn); c->used += rn;
    c->messages += 2; return 0;
}

#define TRANSCRIPT_MAX_LINES 1400
static const char kEarlier[] = "[Earlier conversation is saved in the session file.]\r\r";

/* Make room for an entry of `need` bytes and `lines` total lines by dropping
 * whole leading entries behind one session-file notice. The fewest bytes go;
 * if no cut is enough, only the notice remains. Entry starts move with the
 * text. Returns the new transcript length. */
static size_t transcript_make_room(Chat *c, size_t at, size_t lines, size_t need)
{
    char *t = c->transcript;
    size_t notice = sizeof(kEarlier) - 1, start = 0, cut, keep, crs = 0, i;
    int k = 0, kept = 0;
    if (at >= notice && !memcmp(t, kEarlier, notice)) start = notice;
    for (i = 0; i < start; i++) if (t[i] == 13) crs++;
    for (cut = start;;) {
        while (k < c->entries && c->entry_at[k] < cut) k++;
        if (notice + (at - cut) + need < sizeof(c->transcript) &&
            lines + 2 - crs <= TRANSCRIPT_MAX_LINES && c->entries - k < CHAT_ENTRY_MAX) { keep = cut; break; }
        if (cut >= at) { keep = at; break; }
        while (k < c->entries && c->entry_at[k] <= cut) k++;
        for (i = k < c->entries ? c->entry_at[k] : at; cut < i; cut++) if (t[cut] == 13) crs++;
    }
    for (k = 0; k < c->entries; k++)
        if (c->entry_at[k] >= keep) c->entry_at[kept++] = c->entry_at[k] - keep + notice;
    c->entries = kept;
    memmove(t + notice, t + keep, at - keep + 1);
    memcpy(t, kEarlier, notice);
    return notice + at - keep;
}

/* Bounded transcript display; UTF-8 input, MacRoman/CR output. */
long chat_append_message(Chat *chat, const char *label, const char *text)
{
    static char display[CHAT_TRANSCRIPT_CAP];
    size_t at = strlen(chat->transcript), len, i, lines = 0;
    size_t prefix = (label && *label) ? strlen(label) + 2 : 0; /* "label:\r" */
    chat_tool_forget(chat);
    if (text_to_macroman(text, display, sizeof(display)) < 0) strcpy(display, "[Text exceeds display capacity; see session file.]");
    len = strlen(display);
    for (i = 0; i < at; i++) if (chat->transcript[i] == 13) lines++;
    {
        size_t own_lines = 0;
        for (i = 0; i < len; i++) if (display[i] == 13) own_lines++;
        if (own_lines > 1000 || len > sizeof(chat->transcript) - 256) {
            strcpy(display, "[Text exceeds display limits; see the UTF-8 session file.]");
            len = strlen(display); own_lines = 0;
        }
        lines += own_lines;
    }
    if (at + len + prefix + 8 >= sizeof(chat->transcript) || lines > TRANSCRIPT_MAX_LINES ||
        chat->entries >= CHAT_ENTRY_MAX)
        at = transcript_make_room(chat, at, lines, len + prefix + 8);
    if (prefix + len + 5 >= sizeof(chat->transcript) - at) return -1;
    if (prefix) { strcpy(chat->transcript + at, label); strcat(chat->transcript, ":\r"); }
    strcat(chat->transcript, display); strcat(chat->transcript, "\r\r");
    chat->entry_at[chat->entries++] = at;
    return (long)at;
}
