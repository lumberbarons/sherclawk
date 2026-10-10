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
    reset(); memset(chat.transcript, '\r', 1401); chat.transcript[1401] = 0;
    assert(chat_append_message(&chat, NULL, "x") == (long)strlen(earlier));
    assert(!strcmp(chat.transcript + strlen(earlier), "x\r\r")); history_retained();

    reset(); memset(text, 'L', sizeof(text) - 1); text[sizeof(text) - 1] = 0;
    assert(chat_append_message(&chat, text, "x") == -1);
    assert(!strcmp(chat.transcript, earlier)); history_retained();
}
int main(void)
{
    capping(); puts("chat display checks passed"); return 0;
}
