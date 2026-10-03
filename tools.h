/* Read-only Toolbox tools; model paths remain inside the configured volume. */
#ifndef SHERCLAWK_TOOLS_H
#define SHERCLAWK_TOOLS_H
#include "agent.h"
void tools_execute(const AgentCall *call, char *result, size_t cap);
int tools_validate_path(const char *path, int folder);
#endif
