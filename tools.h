/* Bounded Toolbox tools; mutations require a durable recovery journal. */
#ifndef SHERCLAWK_TOOLS_H
#define SHERCLAWK_TOOLS_H
#include "agent.h"
/* Host diagnostics simulate the File Manager and never resolve guest paths. */
#ifndef SHERCLAWK_HOST
#include <Files.h>
/* Resolve all workspace ancestors without following aliases. */
OSErr tools_resolve(const char *, FSSpec *);
/* The configured workspace as a non-alias folder (bootstrap children). */
OSErr tools_workspace_root(FSSpec *);
#endif
void tools_execute(const AgentCall *call, char *result, size_t cap);
/* Nonzero means stop the run: recording failed or publication is uncertain. */
int tools_execute_recorded(const AgentCall *call, char *result, size_t cap,
                           AgentJournal journal, void *context);
int tools_validate_path(const char *path, int folder);
/* Runtime workspace root (MacRoman, ends in ':'), compiled default unless the
 * app applies the saved preferences. Shared by tools, inspection and builds. */
const char *tools_workspace(void);
void tools_set_workspace(const char *path);
#endif
