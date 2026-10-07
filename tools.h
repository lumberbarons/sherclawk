/* Bounded Toolbox tools; mutations require a durable recovery journal. */
#ifndef SHERCLAWK_TOOLS_H
#define SHERCLAWK_TOOLS_H
#include "agent.h"
/* Host diagnostics simulate the File Manager and never resolve guest paths. */
#ifndef SHERCLAWK_HOST
#include <Files.h>
/* Resolve all workspace ancestors without following aliases. */
OSErr tools_resolve(const char *, FSSpec *);
#endif
void tools_execute(const AgentCall *call, char *result, size_t cap);
/* Nonzero means stop the run: recording failed or publication is uncertain. */
int tools_execute_recorded(const AgentCall *call, char *result, size_t cap,
                           AgentJournal journal, void *context);
int tools_validate_path(const char *path, int folder);
#endif
