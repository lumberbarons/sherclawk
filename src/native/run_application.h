/* Persist successful-build authority and launch only its verified native artifact. */
#ifndef SHERCLAWK_RUN_APPLICATION_H
#define SHERCLAWK_RUN_APPLICATION_H
#include "tools/tools.h"
#include <stdint.h>
/* Typed outcome of launch authorization, so callers never read it back from
 * the run_application JSON. STOPPED: Stop or the deadline ended verification
 * before anything was persisted; FAILED: the artifact was refused or the
 * authority record could not be persisted. */
typedef enum { AUTHORIZE_PENDING, AUTHORIZE_AUTHORIZED, AUTHORIZE_FAILED, AUTHORIZE_STOPPED } AuthorizeResult;
int application_authorize_begin(const char *, const char *, AgentJournal, void *, uint32_t);
/* One fork page or the persist per turn; fills the result buffer only for the
 * journaled model-facing record, which callers must not parse. */
AuthorizeResult application_authorize_step(char *, size_t, uint32_t, int stop);
int run_application_begin(const AgentCall *, char *, size_t, AgentJournal, void *, uint32_t);
/* One fork page, registry entry or process per turn; 2 pending, 0 terminal, 1 stop required. */
int run_application_step(char *, size_t, uint32_t, int);
int quit_application_begin(const AgentCall *, char *, size_t, AgentJournal, void *, uint32_t);
int quit_application_step(char *, size_t, uint32_t, int);
#endif
