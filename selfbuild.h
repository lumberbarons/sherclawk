/* Exclusive self-execution of the current published snapshot, never scanning
 * or replaying old claims. Idle draining outlives the observing chat run. */
#ifndef SHERCLAWK_SELFBUILD_H
#define SHERCLAWK_SELFBUILD_H
#include "jobs.h"
#include "build_plan.h"
int selfbuild_init(void);
void selfbuild_close(void);
/* 1 claimed, 0 external owner/busy/STOP: keep polling; -1 unknown failure. */
int selfbuild_begin(const NativeJob *,const FSSpec *,const BuildPlan *);
void selfbuild_step(uint32_t now,int stop);
void selfbuild_drain(uint32_t now);
int selfbuild_unknown(void);
#endif
