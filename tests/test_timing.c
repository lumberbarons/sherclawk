/* Round and tool timing lines: offsets, unreached phases, first-mark-wins,
 * name sanitising and bounded buffers. */
#include "timing.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_full_round(void)
{
    RoundTiming t; char line[256];
    memset(&t, 0, sizeof(t));
    timing_round_begin(&t, 3, 1000);
    timing_mark(&t, TIMING_INIT, 1002);
    timing_mark(&t, TIMING_CONNECT, 1010);
    timing_mark(&t, TIMING_HANDSHAKE, 1100);
    timing_mark(&t, TIMING_SENT, 1104);
    timing_mark(&t, TIMING_FIRST_BYTE, 1500);
    timing_mark(&t, TIMING_DONE, 1620);
    timing_mark(&t, TIMING_CLOSE, 1740);
    timing_set_bytes(&t, 4096, 812);
    assert(timing_round_format(&t, "ok", line, sizeof(line)) > 0);
    assert(!strcmp(line, "round=3 init=2 connect=10 handshake=100 sent=104 first_byte=500 "
        "done=620 close=740 up=4096 down=812 end=ok"));
}

static void test_token_counts(void)
{
    RoundTiming t; char line[256];
    memset(&t, 0, sizeof(t));
    timing_round_begin(&t, 2, 0);
    timing_set_bytes(&t, 100, 30347);
    timing_set_tokens(&t, -1, -1, 3072);   /* not reported: line unchanged */
    assert(timing_round_format(&t, "abort", line, sizeof(line)) > 0 && !strstr(line, "out="));
    timing_set_tokens(&t, 3072, 3050, 3072);
    assert(timing_round_format(&t, "abort", line, sizeof(line)) > 0);
    assert(strstr(line, " up=100 down=30347 out=3072/3072 reasoning=3050 end=abort"));
    timing_set_tokens(&t, 812, -1, 3072);  /* provider does not split reasoning out */
    assert(timing_round_format(&t, "ok", line, sizeof(line)) > 0);
    assert(strstr(line, " down=30347 out=812/3072 end=ok") && !strstr(line, "reasoning"));
    timing_round_begin(&t, 3, 0);          /* a new round forgets the last counts */
    assert(timing_round_format(&t, "ok", line, sizeof(line)) > 0 && !strstr(line, "out="));
}

static void test_unreached_phases_and_first_mark_wins(void)
{
    RoundTiming t; char line[256];
    memset(&t, 0, sizeof(t));
    timing_round_begin(&t, 1, 50);
    timing_mark(&t, TIMING_INIT, 51);
    timing_mark(&t, TIMING_CONNECT, 60);
    timing_mark(&t, TIMING_CONNECT, 90);   /* repeated observation, must not move it */
    timing_mark(&t, TIMING_CLOSE, 200);
    timing_set_bytes(&t, 10, 0);
    assert(timing_round_format(&t, "abort", line, sizeof(line)) > 0);
    assert(!strcmp(line, "round=1 init=1 connect=10 handshake=- sent=- first_byte=- "
        "done=- close=150 up=10 down=0 end=abort"));
}

static void test_handoff_and_tick_wrap(void)
{
    RoundTiming t; char line[256];
    memset(&t, 0, sizeof(t));
    timing_round_begin(&t, 0, 0xFFFFFFF0u);
    timing_mark(&t, TIMING_INIT, 0x10u);   /* TickCount wrapped */
    assert(timing_round_format(&t, "ok", line, sizeof(line)) > 0);
    assert(!strncmp(line, "handoff init=32 ", 16));
}

static void test_idle_and_small_buffers(void)
{
    RoundTiming t; char line[256], tiny[16];
    memset(&t, 0, sizeof(t));
    assert(timing_round_format(&t, "ok", line, sizeof(line)) < 0);   /* never begun */
    timing_mark(&t, TIMING_INIT, 5);                                 /* ignored while idle */
    timing_round_begin(&t, 2, 5);
    assert(!t.seen[TIMING_INIT]);
    assert(timing_round_format(&t, "ok", tiny, sizeof(tiny)) < 0);
    assert(timing_tool_format(1, "read_text", 0, 1, tiny, 8) < 0);
}

static void test_tool_line_sanitises_name(void)
{
    char line[256];
    assert(timing_tool_format(2, "read_text", 100, 107, line, sizeof(line)) > 0);
    assert(!strcmp(line, "tool=2 name=read_text ticks=7"));
    assert(timing_tool_format(1, "a b\r\"x\xC3", 0, 0xFFFFFFFFu, line, sizeof(line)) > 0);
    assert(!strcmp(line, "tool=1 name=a?b??x? ticks=4294967295"));
    assert(timing_tool_format(1, "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz", 0, 1,
        line, sizeof(line)) > 0);
    assert(strlen(line) < 64);
}

int main(void)
{
    test_full_round();
    test_unreached_phases_and_first_mark_wins();
    test_token_counts();
    test_handoff_and_tick_wrap();
    test_idle_and_small_buffers();
    test_tool_line_sanitises_name();
    puts("timing tests passed");
    return 0;
}
