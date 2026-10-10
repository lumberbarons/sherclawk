#include "display.h"
#include "json.h"
#include <stdio.h>
#include <string.h>

/* Compact OpenCode-style token counts: exact below 1000, else one decimal k/M. */
void FormatTokens(long tokens, char *out, size_t cap)
{
    if (tokens < 1000) snprintf(out, cap, "%ld", tokens);
    else if (tokens < 1000000) {
        long tenths = (tokens + 50) / 100;
        snprintf(out, cap, "%ld.%ldk", tenths / 10, tenths % 10);
    } else {
        long tenths = (tokens + 50000) / 100000;
        snprintf(out, cap, "%ld.%ldM", tenths / 10, tenths % 10);
    }
}

void ComputeLayout(DisplayLayout *layout)
{
    layout->ResponseLabelRect = (DisplayRect){ 52, 10, 68, 200 };
    layout->ResponseRect = (DisplayRect){ 70, 10, 252, 590 };
    layout->ResponseViewRect = layout->ResponseRect; layout->ResponseViewRect.right -= 15;
    layout->PromptLabelRect = (DisplayRect){ 258, 10, 274, 130 };
    layout->PromptHintRect = (DisplayRect){ 258, 410, 274, 590 };
    /* One TextEdit line shorter than the former 82-pixel composer. */
    layout->PromptRect = (DisplayRect){ 278, 10, 348, 590 };
    layout->InfoRect = (DisplayRect){ 356, 10, 398, 590 };
    layout->StatusRect = (DisplayRect){ 358, 18, 376, 320 };
    layout->ModelRect = (DisplayRect){ 358, 328, 376, 582 };
    layout->HistoryRect = (DisplayRect){ 380, 18, 396, 238 };
    layout->MeterRect = (DisplayRect){ 384, 194, 392, 230 };
    layout->UsageRect = (DisplayRect){ 380, 246, 396, 582 };
    layout->ContextRect = (DisplayRect){ 380, 254, 396, 450 };
    layout->CostRect = (DisplayRect){ 380, 466, 396, 582 };
    layout->NewRect = (DisplayRect){ 406, 10, 426, 98 };
    layout->HandoffRect = (DisplayRect){ 406, 108, 426, 220 };
    layout->StopRect = (DisplayRect){ 406, 438, 426, 502 };
    layout->SendRect = (DisplayRect){ 406, 518, 426, 586 };
}

void ToolEventsReset(char *events, size_t cap) { if (cap) events[0] = 0; }
void ToolEventsAppend(char *events, size_t cap, const char *event)
{
    size_t at = strlen(events), n = strlen(event);
    if (!at) {
        if (n < cap) memcpy(events, event, n + 1);
        return;
    }
    if (at + n + 3 > cap) return; /* keep what already fit */
    events[at] = ','; events[at + 1] = ' ';
    memcpy(events + at + 2, event, n + 1);
}
void AppendText(char *out, size_t cap, const char *s)
{
    size_t at, n;
    if (!cap) return;
    at = strlen(out); n = strlen(s);
    if (at >= cap || n >= cap - at) return;
    memcpy(out + at, s, n + 1);
}

/* "• name(arg: value, ...)" from the recorded arguments, the same JSON the
 * tool layer parses. Long strings and nesting are abbreviated, and the whole
 * header is display-bounded. */
void RenderToolCall(const AgentCall *call, char *out, size_t cap)
{
    static JsonToken tokens[256];
    static char value[132];
    size_t i;
    int parsed, first = 1;

    if (!cap) return;
    snprintf(out, cap, "\xE2\x80\xA2 %s(", call->name);
    parsed = call->arguments[0] ? json_parse(call->arguments, strlen(call->arguments), tokens, 256) : -1;
    if (parsed < 1 || tokens[0].type != JSON_OBJECT) {
        size_t at = strlen(out), n = strlen(call->arguments);
        if (at < cap - 1 && n > cap - at - 2) n = cap - at - 2;
        if (at < cap - 1) { memcpy(out + at, call->arguments, n); out[at + n] = 0; }
    } else {
        for (i = 1; i < (size_t)tokens[0].next; i = (size_t)tokens[i + 1].next) {
            int v = (int)i + 1;
            char key[64];
            if (!first) AppendText(out, cap, ", ");
            first = 0;
            if (json_string(call->arguments, tokens, (int)i, key, sizeof(key)) < 0) strcpy(key, "?");
            AppendText(out, cap, key);
            AppendText(out, cap, ": ");
            switch (tokens[v].type) {
            case JSON_STRING:
                if (json_string(call->arguments, tokens, v, value, sizeof(value)) < 0) strcpy(value, "<long>");
                AppendText(out, cap, "\""); AppendText(out, cap, value); AppendText(out, cap, "\"");
                break;
            case JSON_PRIMITIVE: {
                int n = tokens[v].end - tokens[v].start;
                if (n > 40) n = 40;
                memcpy(value, call->arguments + tokens[v].start, (size_t)n); value[n] = 0;
                AppendText(out, cap, value);
                break;
            }
            default:
                AppendText(out, cap, tokens[v].type == JSON_ARRAY ? "[...]" : "{...}");
                break;
            }
            if (cap < 12 || strlen(out) > cap - 12) { AppendText(out, cap, ", ..."); break; }
        }
    }
    AppendText(out, cap, ")");
}

int ModelLookupAllowed(const char *model)
{
    size_t i;
    if (!*model) return 0;
    for (i = 0; model[i]; i++) {
        unsigned char c = (unsigned char)model[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' ||
              c == '/' || c == '-')) return 0;
    }
    return 1;
}

int model_id_valid(const char *model)
{
    size_t i;
    if (!*model) return 0;
    for (i = 0; model[i]; i++)
        if ((unsigned char)model[i] <= 32 || (unsigned char)model[i] >= 127) return 0;
    return 1;
}
int prompt_is_blank(const char *prompt)
{
    size_t i;
    for (i = 0; prompt[i]; i++)
        if (prompt[i] != ' ' && prompt[i] != '\r' && prompt[i] != '\t') return 0;
    return 1;
}
void FormatPauseReason(int rounds, int tools, int max_rounds, int max_tools,
                       size_t used, char *out, size_t cap)
{
    snprintf(out, cap,
        "Run paused after %d model rounds and %d tools (configured limits: %d rounds, %d tools). "
        "History is %lu%% full. Send Continue to resume.",
        rounds, tools, max_rounds, max_tools,
        (unsigned long)(used * 100 / AGENT_HISTORY_CAP));
}
short ComputeMaxScroll(short line_height, short line_count, int view_height)
{
    long pixels;
    if (line_height <= 0) line_height = 12;
    pixels = (long)line_count * line_height - view_height;
    if (pixels < 0) return 0;
    return (short)(pixels > 32767L ? 32767L : pixels);
}
short ClampScroll(long pixels, short maximum)
{
    if (pixels < 0) pixels = 0;
    if (pixels > maximum) pixels = maximum;
    return (short)pixels;
}
short ResponsePageHeight(short line_height, int view_height)
{
    int visible_lines;
    if (line_height <= 0) line_height = 12;
    visible_lines = view_height / line_height;
    return visible_lines > 1 ? (short)((visible_lines - 1) * line_height) : line_height;
}
