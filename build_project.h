/* Revision-bound native builds; callers service events between steps. */
#ifndef SHERCLAWK_BUILD_PROJECT_H
#define SHERCLAWK_BUILD_PROJECT_H
#include "tools.h"
#include "jobs.h"
#include "build_plan.h"
int build_project_begin(const AgentCall *, char *, size_t, AgentJournal, void *, uint32_t);
/* 2 pending, 0 terminal, 1 terminal requiring the agent to stop. */
int build_project_step(char *, size_t, uint32_t, int stop);
void build_project_log(const AgentCall *, char *, size_t);
/* Pure descriptor/recipe validation, also used by host checks. */
int build_project_recipe(const char *, char *, size_t);
#endif
