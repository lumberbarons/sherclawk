/* Exclusive native MPW/ToolServer execution of the current published snapshot,
 * never scanning or replaying old claims. Idle draining outlives the observing
 * chat run. The queue's worker-lock folder and STOP marker keep their original
 * names: they are the claim exclusivity and halt switch for this executor. */
#ifndef SHERCLAWK_SELFBUILD_H
#define SHERCLAWK_SELFBUILD_H
#include "jobs.h"
#include "build_plan.h"
int selfbuild_init(void);
void selfbuild_close(void);
/* Why a build did or did not start. Only UNCERTAIN may have changed the queue
 * (the lock was taken); every other refusal leaves it untouched. */
typedef enum {
    SELFBUILD_STARTED,      /* claimed; selfbuild_step now drives it */
    SELFBUILD_BUSY,         /* an earlier ToolServer command has not drained */
    SELFBUILD_BLOCKED,      /* worker-lock or STOP held, or queue unusable */
    SELFBUILD_UNAVAILABLE,  /* ToolServer channel never initialized */
    SELFBUILD_UNCERTAIN     /* lock taken, setup failed; no replay */
} SelfBuildStart;
/* Side-effect-free precheck of the refusals above: 0 if a claim may proceed,
 * else SELFBUILD_BUSY/BLOCKED/UNAVAILABLE. Races are still decided by begin. */
int selfbuild_check(const FSSpec *queue);
SelfBuildStart selfbuild_begin(const NativeJob *,const FSSpec *,const BuildPlan *);
void selfbuild_step(uint32_t now,int stop);
void selfbuild_drain(uint32_t now);
int selfbuild_unknown(void);
#endif
