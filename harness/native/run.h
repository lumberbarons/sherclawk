#ifndef HARNESS_RUN_H
#define HARNESS_RUN_H
#include "contract.h"
typedef struct {
    void *ctx;
    unsigned long (*now)(void *);
    int (*request)(void *, HRequest *); /* 1 valid, 0 absent/invalid */
    int (*claim)(void *, const HRequest *); /* 1 claimed, 0 duplicate, -1 uncertain */
    int (*configure)(void *, const HRequest *);
    int (*launch)(void *, const HRequest *); /* 0 is uncertain, never replay */
    int (*report)(void *, const HRequest *, HReport *); /* 0 missing, 1 valid, -1 bad */
    int (*present)(void *); /* 1 present, 0 absent, -1 uncertain */
    int (*stopped)(void *, const HRequest *);
    int (*publish)(void *, const HReport *);
    int (*cleanup)(void *);
} HRunHost;
enum { H_IDLE, H_CONFIGURE, H_LAUNCH, H_OBSERVE, H_PUBLISH, H_CLEANUP, H_QUARANTINED };
typedef struct {
    int state;
    char slot[6], boot[33];
    HReport report;
} HRun;
void h_run_init(HRun *, const char *, const char *);
void h_run_step(HRun *, const HRunHost *);
#endif
