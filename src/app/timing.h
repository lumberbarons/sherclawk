/* Per-round and per-tool tick timings for the share log. Pure bookkeeping so
 * it compiles on the host; callers supply TickCount() values. Lines carry
 * only numbers and a sanitized tool name, never request or response text. */
#ifndef SHERCLAWK_TIMING_H
#define SHERCLAWK_TIMING_H
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TIMING_INIT,        /* Open Transport initialised, before the TLS context */
    TIMING_CONNECT,     /* TCP up, TLS handshake started */
    TIMING_HANDSHAKE,   /* TLS connected */
    TIMING_SENT,        /* request fully written */
    TIMING_FIRST_BYTE,  /* first response byte read */
    TIMING_DONE,        /* response complete */
    TIMING_CLOSE,       /* context and Open Transport torn down */
    TIMING_PHASES
} TimingPhase;

typedef struct {
    int      active, round;                 /* round <= 0 marks the handoff request */
    uint32_t start, at[TIMING_PHASES];
    unsigned char seen[TIMING_PHASES];
    int tokens_seen;
    unsigned long up, down;
    long completion, reasoning, limit;      /* completion < 0: not reported */
} RoundTiming;

void timing_round_begin(RoundTiming *t, int round, uint32_t now);
/* The first mark of a phase wins; later marks of the same phase are ignored. */
void timing_mark(RoundTiming *t, TimingPhase phase, uint32_t now);
void timing_set_bytes(RoundTiming *t, unsigned long up, unsigned long down);
/* Provider-reported completion tokens against the request cap; reasoning < 0
 * when the provider does not split it out. A negative completion is ignored. */
void timing_set_tokens(RoundTiming *t, long completion, long reasoning, long limit);
/* Offsets are ticks since the round began; a phase never reached prints "-".
 * Returns the length, or -1 if the buffer is too small or the round is idle. */
int timing_round_format(const RoundTiming *t, const char *outcome, char *out, size_t cap);
int timing_tool_format(int index, const char *name, uint32_t start, uint32_t end, char *out, size_t cap);
#endif
