/* Bounded Toolbox tools; mutations require a durable recovery journal. */
#ifndef SHERCLAWK_TOOLS_H
#define SHERCLAWK_TOOLS_H
#include "agent.h"
#include <stdint.h>
#include "text_limits.h"
int read_text_begin(const AgentCall *, char *, size_t, AgentJournal, void *, uint32_t);
int read_text_step(char *, size_t, uint32_t, int);
int edit_text_begin(const AgentCall *, char *, size_t, AgentJournal, void *, uint32_t);
int edit_text_step(char *, size_t, uint32_t, int);
/* Host diagnostics simulate the File Manager and never resolve guest paths. */
#ifndef SHERCLAWK_HOST
#include <Files.h>
/* Resolve all workspace ancestors without following aliases. */
OSErr tools_resolve(const char *, FSSpec *);
/* The configured workspace as a non-alias folder (bootstrap children). */
OSErr tools_workspace_root(FSSpec *);
/* Catalog revision token for a file: catalog identity, modification date and
 * both fork sizes. Emitted by list_files/get_file_info and required by
 * move_to_trash; it detects a changed or replaced target, not a rewritten one. */
void tools_catalog_revision(const CInfoPBRec *, char *out, size_t cap);
/* Bounded depth-first catalog walk for the directory with the wanted ID,
 * filling its workspace-relative MacRoman path with a trailing colon. Stops at
 * *budget entries or depth 8; *complete reports an incomplete walk. */
int tools_find_dir(const FSSpec *spec, long dir, const char *prefix, long wanted,
                   char *found, size_t cap, int depth, int *budget, int *complete);
#endif
int tools_execute(const AgentCall *call, char *result, size_t cap);
/* 2 pending, 0 completed, 1 stop (recording failed or uncertain outcome). */
int tools_execute_recorded(const AgentCall *call, char *result, size_t cap,
                           AgentJournal journal, void *context);
const char *tools_text_phase(void);
int tools_text_step(char *, size_t, uint32_t, int stop);
int tools_validate_path(const char *path, int folder);
/* Runtime workspace root (MacRoman, ends in ':'), compiled default unless the
 * app applies the saved preferences. Shared by tools, inspection and builds. */
const char *tools_workspace(void);
/* AGENTS.md loading. The file is <folder>:AGENTS.md below the workspace, or
 * AGENTS.md at its root when folder is "". It is plain MacRoman/CR TEXT like
 * every workspace text file, read once, whole, and delivered as UTF-8 with LF. */
#define TOOLS_INSTRUCTIONS_ABSENT 0     /* no such file, or an empty one */
#define TOOLS_INSTRUCTIONS_LOADED 1
#define TOOLS_INSTRUCTIONS_TRUNCATED 2  /* cut on a line to fit AGENT_INSTRUCTIONS_CAP, marker appended */
#define TOOLS_INSTRUCTIONS_UNUSABLE (-1) /* binary, alias, folder, fork, changed while read, or cap too small */
/* out needs AGENT_INSTRUCTIONS_CAP + 1 bytes; it is empty unless a file was
 * loaded. hash names the raw bytes read, for the journal. */
int tools_read_instructions(const char *folder, char *out, size_t cap, unsigned long *hash);
/* The project folder a tool call works in: the first component of its path or
 * root argument. Returns 1 and copies it, or 0 (name emptied) when the call
 * names none or it does not fit cap. */
int tools_call_project(const AgentCall *call, char *name, size_t cap);
void tools_set_workspace(const char *path);
#endif
