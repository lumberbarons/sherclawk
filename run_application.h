/* Persist successful-build authority and launch only its verified native artifact. */
#ifndef SHERCLAWK_RUN_APPLICATION_H
#define SHERCLAWK_RUN_APPLICATION_H
#include "tools.h"
#include <stdint.h>
int application_authorize_begin(const char *, const char *, AgentJournal, void *, uint32_t);
int run_application_begin(const AgentCall *, char *, size_t, AgentJournal, void *, uint32_t);
/* One <=1 KiB fork read per turn; 2 pending, 0 terminal, 1 stop required. */
int run_application_step(char *, size_t, uint32_t, int);
#endif
