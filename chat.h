/* Bounded MacRoman/CR display transcript; the agent owns wire history. */
#ifndef SHERCLAWK_CHAT_H
#define SHERCLAWK_CHAT_H
#include <stddef.h>
#define CHAT_REQUEST_CAP 1081344
#define CHAT_RESPONSE_CAP 65536
#define CHAT_TRANSCRIPT_CAP 30001
#define CHAT_PROMPT_CAP 8193
#define CHAT_MODEL_CAP 256
#define CHAT_ENTRY_MAX 512
typedef struct {
    char transcript[CHAT_TRANSCRIPT_CAP];
    /* Where each transcript entry starts, oldest first, so display capping
     * drops whole entries; any session-file notice sits before the first. */
    size_t entry_at[CHAT_ENTRY_MAX];
    int entries;
    /* The last transcript entry, when it is a tool call line that a following
     * call to the same tool may fold into: where it starts, where the
     * transcript ended after it (0 when there is none), its tool and run. */
    size_t tool_at, tool_end;
    int tool_count;
    char tool_name[64];
} Chat;
void chat_reset(Chat *chat);
/* Returns the entry offset, or -1 if its label cannot fit. The oldest whole
 * entries may be replaced by a session-file notice. */
long chat_append_message(Chat *chat, const char *label, const char *text);
/* Collapsible tool lines. chat_tool_note marks the entry that starts at start
 * (the transcript's newest) as one call of tool name; chat_tool_forget drops
 * the mark. chat_tool_collapse returns 1 after rewriting that entry as
 * "<bullet> name xN" when the transcript still ends with it and the next call is
 * to the same tool, and 0 (nothing changed) when the caller must append. */
void chat_tool_note(Chat *chat, size_t start, const char *name);
void chat_tool_forget(Chat *chat);
int chat_tool_collapse(Chat *chat, const char *name);
#endif
