#include "timing.h"
#include <stdio.h>
#include <string.h>

#define TOOL_NAME_MAX 40

static const char *const kNames[TIMING_PHASES] = {
    "init", "connect", "handshake", "sent", "first_byte", "done", "close"
};

void timing_round_begin(RoundTiming *t, int round, uint32_t now)
{
    memset(t, 0, sizeof(*t));
    t->active = 1; t->round = round; t->start = now;
}

void timing_mark(RoundTiming *t, TimingPhase phase, uint32_t now)
{
    if (!t->active || (int)phase < 0 || phase >= TIMING_PHASES || t->seen[phase]) return;
    t->seen[phase] = 1; t->at[phase] = now;
}

void timing_set_bytes(RoundTiming *t, unsigned long up, unsigned long down)
{
    t->up = up; t->down = down;
}

void timing_set_tokens(RoundTiming *t, long completion, long reasoning, long limit)
{
    if (completion < 0) return;
    t->completion = completion; t->reasoning = reasoning; t->limit = limit; t->tokens_seen = 1;
}

int timing_round_format(const RoundTiming *t, const char *outcome, char *out, size_t cap)
{
    size_t used;
    int i, n;
    if (!t->active || !cap) return -1;
    if (t->round > 0) n = snprintf(out, cap, "round=%d", t->round);
    else n = snprintf(out, cap, "handoff");
    if (n < 0 || (size_t)n >= cap) return -1;
    used = (size_t)n;
    for (i = 0; i < TIMING_PHASES; i++) {
        if (t->seen[i])
            n = snprintf(out + used, cap - used, " %s=%lu", kNames[i], (unsigned long)(uint32_t)(t->at[i] - t->start));
        else n = snprintf(out + used, cap - used, " %s=-", kNames[i]);
        if (n < 0 || (size_t)n >= cap - used) return -1;
        used += (size_t)n;
    }
    n = snprintf(out + used, cap - used, " up=%lu down=%lu", t->up, t->down);
    if (n < 0 || (size_t)n >= cap - used) return -1;
    used += (size_t)n;
    if (t->tokens_seen) {
        n = snprintf(out + used, cap - used, " out=%ld/%ld", t->completion, t->limit);
        if (n < 0 || (size_t)n >= cap - used) return -1;
        used += (size_t)n;
        if (t->reasoning >= 0) {
            n = snprintf(out + used, cap - used, " reasoning=%ld", t->reasoning);
            if (n < 0 || (size_t)n >= cap - used) return -1;
            used += (size_t)n;
        }
    }
    n = snprintf(out + used, cap - used, " end=%s", outcome);
    if (n < 0 || (size_t)n >= cap - used) return -1;
    return (int)(used + (size_t)n);
}

int timing_tool_format(int index, const char *name, uint32_t start, uint32_t end, char *out, size_t cap)
{
    char safe[TOOL_NAME_MAX + 1];
    size_t i;
    int n;
    for (i = 0; name[i] && i < TOOL_NAME_MAX; i++) {
        unsigned char c = (unsigned char)name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        safe[i] = ok ? (char)c : '?';
    }
    safe[i] = 0;
    if (!cap) return -1;
    n = snprintf(out, cap, "tool=%d name=%s ticks=%lu", index, safe, (unsigned long)(uint32_t)(end - start));
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}
