/* Pure display behavior shared by the app and native diagnostics. */
#include "display.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void formatting(void)
{
    char out[256], tiny[2] = "x", events[8];
    const long counts[] = {0, 999, 1000, 1049, 1050, 999999, 1000000, 1050000};
    const char *expected[] = {"0", "999", "1.0k", "1.0k", "1.1k", "1000.0k", "1.0M", "1.1M"};
    size_t i;
    for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        FormatTokens(counts[i], out, sizeof(out));
        assert(!strcmp(out, expected[i]));
    }
    FormatTokens(1234, tiny, sizeof(tiny)); assert(!strcmp(tiny, "1"));
    AppendText(tiny, sizeof(tiny), "a"); assert(!strcmp(tiny, "1"));
    tiny[0] = 0; AppendText(tiny, sizeof(tiny), "a"); assert(!strcmp(tiny, "a"));
    ToolEventsReset(events, sizeof(events));
    ToolEventsAppend(events, sizeof(events), "abc");
    ToolEventsAppend(events, sizeof(events), "de"); assert(!strcmp(events, "abc, de"));
    ToolEventsAppend(events, sizeof(events), "f"); assert(!strcmp(events, "abc, de"));
    ToolEventsReset(events, sizeof(events));
    ToolEventsAppend(events, sizeof(events), "12345678"); assert(!*events);
    ToolEventsAppend(events, sizeof(events), "1234567"); assert(!strcmp(events, "1234567"));
    FormatPauseReason(3, 7, 4, 8, AGENT_HISTORY_CAP / 2, out, sizeof(out));
    assert(!strcmp(out, "Run paused after 3 model rounds and 7 tools (configured limits: 4 rounds, 8 tools). History is 50% full. Send Continue to resume."));
    FormatPauseReason(0, 0, 1, 1, 0, tiny, sizeof(tiny)); assert(!strcmp(tiny, "R"));
}

static void validation(void)
{
    unsigned int c;
    char model[] = {'a', 0, 0};
    assert(!model_id_valid("")); assert(!ModelLookupAllowed(""));
    assert(model_id_valid("vendor/model-v1.2_test:free"));
    assert(ModelLookupAllowed("vendor/model-v1.2_test:free"));
    assert(model_id_valid("vendor/model?x")); assert(!ModelLookupAllowed("vendor/model?x"));
    for (c = 1; c <= 255; c++) {
        model[1] = (char)c;
        assert(model_id_valid(model) == (c > 32 && c < 127));
    }
    assert(prompt_is_blank("")); assert(prompt_is_blank(" \r\t "));
    assert(!prompt_is_blank(" \r\tx")); assert(!prompt_is_blank("\n"));
    assert(!prompt_is_blank("\x8e"));
}

static void rendering(void)
{
    static AgentCall call;
    char out[400], small[16];
    size_t cap;
    strcpy(call.name, "read_text");
    strcpy(call.arguments, "{\"path\":\"foo\\rbar\",\"offset\":12,\"ok\":true,\"items\":[1],\"nested\":{\"x\":1}}");
    RenderToolCall(&call, out, sizeof(out));
    assert(!strcmp(out, "\xE2\x80\xA2 read_text(path: \"foo\rbar\", offset: 12, ok: true, items: [...], nested: {...})"));
    strcpy(call.arguments, "{}"); RenderToolCall(&call, out, sizeof(out));
    assert(!strcmp(out, "\xE2\x80\xA2 read_text()"));
    strcpy(call.arguments, "bad json"); RenderToolCall(&call, out, sizeof(out));
    assert(!strcmp(out, "\xE2\x80\xA2 read_text(bad json)"));
    strcpy(call.arguments, "{\"s\":\""); memset(call.arguments + 6, 'a', 132);
    strcpy(call.arguments + 138, "\"}"); RenderToolCall(&call, out, sizeof(out));
    assert(!strcmp(out, "\xE2\x80\xA2 read_text(s: \"<long>\")"));
    for (cap = 1; cap < sizeof(small); cap++) {
        memset(small, 'X', sizeof(small)); RenderToolCall(&call, small, cap);
        assert(memchr(small, 0, cap)); assert(small[cap] == 'X');
    }
    strcpy(call.arguments, "not an object and deliberately too long");
    for (cap = 1; cap < sizeof(small); cap++) {
        memset(small, 'X', sizeof(small)); RenderToolCall(&call, small, cap);
        assert(memchr(small, 0, cap)); assert(small[cap] == 'X');
    }
    RenderToolCall(&call, NULL, 0); AppendText(NULL, 0, "x");
}

static void scrolling(void)
{
    DisplayLayout l;
    ComputeLayout(&l);
    assert(l.ResponseRect.top == 70 && l.ResponseRect.bottom == 252);
    assert(l.ResponseRect.left == 10 && l.ResponseRect.right == 590);
    assert(l.ResponseViewRect.right == 575 && l.ResponseViewRect.top == 70);
    assert(l.PromptRect.top == 278 && l.PromptRect.bottom == 348);
    assert(l.PromptRect.left == 10 && l.PromptRect.right == 590);
    assert(l.SendRect.left == 518 && l.SendRect.bottom == 426);
    assert(l.StopRect.right < l.SendRect.left && l.NewRect.right < l.HandoffRect.left);
    assert(l.ResponseRect.bottom < l.PromptLabelRect.top && l.PromptRect.bottom < l.InfoRect.top);
    assert(ComputeMaxScroll(12, 0, 182) == 0);
    assert(ComputeMaxScroll(12, 15, 182) == 0);
    assert(ComputeMaxScroll(12, 16, 182) == 10);
    assert(ComputeMaxScroll(12, 100, 182) == 1018);
    assert(ComputeMaxScroll(12, 1500, 182) == 17818);
    assert(ComputeMaxScroll(12, 32767, 182) == 32767);
    assert(ComputeMaxScroll(0, 100, 182) == 1018);
    assert(ResponsePageHeight(12, 182) == 168);
    assert(ResponsePageHeight(12, 24) == 12);
    assert(ResponsePageHeight(12, 0) == 12);
    assert(ResponsePageHeight(-1, 182) == 168);
    assert(ClampScroll(-100000, 1018) == 0);
    assert(ClampScroll(100000, 1018) == 1018);
    assert(ClampScroll(509, 1018) == 509);
    assert(ClampScroll(1, 0) == 0);
}
int main(void)
{
    formatting(); validation(); rendering(); scrolling();
    puts("display checks passed"); return 0;
}
