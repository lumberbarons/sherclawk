/* Bounded native transcript display, shared by the UI and diagnostic rigs. */
#include "chat.h"
#include "text.h"
#include <stdio.h>
#include <string.h>

void chat_reset(Chat *c) { memset(c, 0, sizeof(*c)); }
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
