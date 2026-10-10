#include "chat.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static Chat chat;
static char text[CHAT_TRANSCRIPT_CAP + 1];
static const char earlier[] = "[Earlier conversation is saved in the session file.]\r\r";
static const char limits[] = "[Text exceeds display limits; see the UTF-8 session file.]\r\r";

static void reset(void)
{
    chat_reset(&chat);
    strcpy(chat.history, "history sentinel"); chat.used = 17;
    chat.offsets[0] = 5; chat.messages = 1;
}
static void history_retained(void)
{
    assert(!strcmp(chat.history, "history sentinel"));
    assert(chat.used == 17 && chat.offsets[0] == 5 && chat.messages == 1);
}
/* Every recorded entry starts right after a blank line (or the notice), in order. */
static void entries_aligned(void)
{
    size_t notice = !strncmp(chat.transcript, earlier, strlen(earlier)) ? strlen(earlier) : 0;
    int k;
    assert(chat.entries > 0 && chat.entries <= CHAT_ENTRY_MAX);
    assert(chat.entry_at[0] == notice);
    for (k = 1; k < chat.entries; k++)
        assert(chat.entry_at[k] > chat.entry_at[k - 1] + 1 && chat.transcript[chat.entry_at[k] - 1] == 13 &&
               chat.transcript[chat.entry_at[k] - 2] == 13);
    assert(chat.entry_at[chat.entries - 1] < strlen(chat.transcript));
}
static void capping(void)
{
    long at;
    reset();
    assert(chat_append_message(&chat, "You", "caf\xc3\xa9\nnext") == 0);
    assert(!strcmp(chat.transcript, "You:\rcaf\x8e\rnext\r\r"));
    at = (long)strlen(chat.transcript);
    assert(chat_append_message(&chat, NULL, "tool()") == at);
    chat_tool_note(&chat, (size_t)at, "tool"); assert(chat.tool_end != 0);
    assert(chat_append_message(&chat, "", "") > at); assert(chat.tool_end == 0);
    history_retained();

    reset(); memset(text, 'x', sizeof(text) - 1); text[sizeof(text) - 1] = 0;
    assert(chat_append_message(&chat, NULL, text) == 0);
    assert(!strcmp(chat.transcript, "[Text exceeds display capacity; see session file.]\r\r"));
    reset(); text[CHAT_TRANSCRIPT_CAP - 256] = 0;
    assert(chat_append_message(&chat, NULL, text) == 0);
    assert(strlen(chat.transcript) == CHAT_TRANSCRIPT_CAP - 254);
    memset(text, 'n', 256); text[256] = 0;
    assert(chat_append_message(&chat, "Notice", text) == (long)strlen(earlier));
    assert(!strncmp(chat.transcript, earlier, strlen(earlier))); history_retained();
    reset(); memset(text, 'x', sizeof(text) - 1); text[CHAT_TRANSCRIPT_CAP - 256] = 'x'; text[CHAT_TRANSCRIPT_CAP - 255] = 0;
    assert(chat_append_message(&chat, NULL, text) == 0);
    assert(!strcmp(chat.transcript, limits));

    reset(); memset(text, '\r', 1001); text[1000] = 0;
    assert(chat_append_message(&chat, NULL, text) == 0);
    assert(strlen(chat.transcript) == 1002);
    text[1000] = '\r'; text[1001] = 0;
    reset(); assert(chat_append_message(&chat, NULL, text) == 0);
    assert(!strcmp(chat.transcript, limits));
    reset(); memset(chat.transcript, '\r', 1400); chat.transcript[1400] = 0;
    assert(chat_append_message(&chat, NULL, "x") == 1400);
    /* Over the line cap, only as many leading entries go as needed. */
    reset();
    for (at = 0; at < 350; at++) assert(chat_append_message(&chat, NULL, "\n\n") >= 0);
    assert(chat_append_message(&chat, NULL, "x") == 1400);
    assert(!strstr(chat.transcript, "Earlier"));
    assert(chat_append_message(&chat, NULL, "y") == (long)strlen(earlier) + 1399);
    assert(!strncmp(chat.transcript, earlier, strlen(earlier)));
    assert(!strcmp(chat.transcript + strlen(earlier) + 1399, "y\r\r")); entries_aligned(); history_retained();
    /* Text no entry was recorded for is a single entry. */
    reset(); memset(chat.transcript, '\r', 1401); chat.transcript[1401] = 0;
    assert(chat_append_message(&chat, NULL, "x") == (long)strlen(earlier));
    assert(!strcmp(chat.transcript + strlen(earlier), "x\r\r")); entries_aligned(); history_retained();

    reset(); memset(text, 'L', sizeof(text) - 1); text[sizeof(text) - 1] = 0;
    assert(chat_append_message(&chat, text, "x") == -1);
    assert(!strcmp(chat.transcript, earlier)); history_retained();
}
/* Entry bodies carry no blank line, so each "\r\r" ends exactly one entry. */
static void fill_entry(const char *label, char ch, size_t bytes)
{
    memset(text, ch, bytes); text[bytes] = 0;
    assert(chat_append_message(&chat, label, text) >= 0);
}
static void dropping_oldest(void)
{
    long at;
    size_t before, i;
    reset();
    fill_entry("First", 'a', 5000); fill_entry("Second", 'b', 5000);
    fill_entry("Third", 'c', 5000); fill_entry("Fourth", 'd', 5000);
    fill_entry("Fifth", 'e', 5000);
    assert(!strstr(chat.transcript, "Earlier"));
    before = strlen(chat.transcript);
    memset(text, 'f', 5000); text[5000] = 0;
    at = chat_append_message(&chat, "Sixth", text);
    /* Only the oldest entry goes; the rest stay readable behind one notice. */
    assert(!strncmp(chat.transcript, earlier, strlen(earlier)));
    assert(!strstr(chat.transcript, "First:"));
    assert(!strncmp(chat.transcript + strlen(earlier), "Second:\r", 8));
    assert(strstr(chat.transcript, "Fifth:\r") && strstr(chat.transcript, "Sixth:\r"));
    assert(strlen(chat.transcript) < CHAT_TRANSCRIPT_CAP && strlen(chat.transcript) > before - 5009);
    assert(at == (long)(strlen(chat.transcript) - strlen("Sixth:\r") - 5000 - 2));
    assert(!strcmp(chat.transcript + at + 7 + 5000, "\r\r")); entries_aligned(); history_retained();

    /* The notice is never stacked, however often the window rolls. */
    for (i = 0; i < 12; i++) fill_entry("More", 'g', 6000);
    assert(!strncmp(chat.transcript, earlier, strlen(earlier)));
    assert(!strstr(chat.transcript + 1, "[Earlier"));
    assert(strlen(chat.transcript) < CHAT_TRANSCRIPT_CAP); history_retained();
}
static void dropping_for_lines(void)
{
    size_t i;
    reset();
    for (i = 0; i < 600; i++) strcpy(text + 2 * i, "l\n");
    text[1200] = 0;
    assert(chat_append_message(&chat, "First", text) == 0);
    assert(chat_append_message(&chat, "Second", text) >= 0);
    assert(!strstr(chat.transcript, "Earlier"));
    assert(chat_append_message(&chat, "Third", text) >= 0);
    assert(!strncmp(chat.transcript, earlier, strlen(earlier)));
    assert(!strstr(chat.transcript, "First:"));
    assert(strstr(chat.transcript, "Second:\r") && strstr(chat.transcript, "Third:\r"));
    history_retained();
}
/* A reply with blank lines of its own is still one entry: it goes whole or stays whole. */
static void dropping_whole_entries(void)
{
    size_t i;
    reset();
    for (i = 0; i < 60; i++) {
        memset(text, 'p', 400); memcpy(text + 400, "\n\n", 2); memset(text + 402, 'q', 400); text[802] = 0;
        assert(chat_append_message(&chat, i % 2 ? "Assistant" : "You", text) >= 0);
    }
    assert(!strncmp(chat.transcript, earlier, strlen(earlier)));
    assert(!strncmp(chat.transcript + strlen(earlier), "You:\r", 5) ||
           !strncmp(chat.transcript + strlen(earlier), "Assistant:\r", 11));
    entries_aligned(); history_retained();
}
static void dropping_for_entry_count(void)
{
    int i;
    reset();
    for (i = 0; i < CHAT_ENTRY_MAX; i++) assert(chat_append_message(&chat, NULL, "n") >= 0);
    assert(chat.entries == CHAT_ENTRY_MAX && !strstr(chat.transcript, "Earlier"));
    assert(chat_append_message(&chat, NULL, "last") >= 0);
    assert(!strncmp(chat.transcript, earlier, strlen(earlier)));
    assert(chat.entries == CHAT_ENTRY_MAX);
    assert(!strcmp(chat.transcript + chat.entry_at[chat.entries - 1], "last\r\r")); entries_aligned(); history_retained();
}
int main(void)
{
    capping(); dropping_whole_entries(); dropping_for_entry_count(); dropping_oldest(); dropping_for_lines(); puts("chat display checks passed"); return 0;
}
