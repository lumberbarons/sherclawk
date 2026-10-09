/* Read-only OS 9 inspection tools (see ADR-0003). */
#ifndef SHERCLAWK_INSPECT_H
#define SHERCLAWK_INSPECT_H
#include "agent.h"
/* Dispatches get_file_info, resolve_alias, list_processes, list_fonts,
 * measure_text, list_resources and read_resource. Read-only: no journal. */
void inspect_execute(const AgentCall *call, char *out, size_t cap);
#endif
