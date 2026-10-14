/* Bounded Toolbox tools; mutations require a durable recovery journal. */
#ifndef SHERCLAWK_TOOLS_H
#define SHERCLAWK_TOOLS_H
#include "agent/agent.h"
#include "agent/json.h"
#include <stdint.h>
#include "agent/text_limits.h"

/* ── Tool registry ───────────────────────────────────────────────────
 * One row per model-facing tool: the single definition site of its name,
 * wire schema and classification. The rows are pure data on purpose: the
 * host suites that link agent.c alone cannot link the tool executors or
 * the File Manager model, so dispatch stays with the module that owns the
 * code (tools.c's id switch, main.c's pending states), keyed by id. */
enum ToolId {
    TOOL_get_environment, TOOL_list_files, TOOL_read_text, TOOL_search_text,
    TOOL_write_text, TOOL_edit_text, TOOL_create_folder, TOOL_move_to_trash,
    TOOL_create_project, TOOL_build_project, TOOL_read_build_log,
    TOOL_run_application, TOOL_quit_application, TOOL_get_file_info,
    TOOL_resolve_alias, TOOL_list_processes, TOOL_list_fonts,
    TOOL_measure_text, TOOL_list_resources, TOOL_read_resource,
    TOOL_view_image, TOOL_copy_file, TOOL_read_mcp_result
};
typedef struct {
    enum ToolId id;
    const char *name;
    const char *schema_json; /* one {"type":"function","function":{...}} object */
    unsigned mutates : 1; /* pre-checked: journal + sealed build-evidence paths */
    unsigned pending : 1; /* finishes over event-loop steps; main.c drives it */
    const char *mutation_argument; /* NULL means path; copy_file writes destination */
} ToolDef;
extern const ToolDef kToolDefs[];
/* Row by table position (NULL past the end), the table size, and the row a
 * model call names (NULL when the tool is not installed). */
const ToolDef *tools_at(size_t i);
size_t tools_count(void);
const ToolDef *tools_lookup(const char *name);

int read_text_begin(const AgentCall *, char *, size_t, AgentJournal, void *, uint32_t);
int read_text_step(char *, size_t, uint32_t, int);
int edit_text_begin(const AgentCall *, char *, size_t, AgentJournal, void *, uint32_t);
int edit_text_step(char *, size_t, uint32_t, int);
int copy_file_begin(const AgentCall *, char *, size_t, AgentJournal, void *, uint32_t);
int copy_file_step(char *, size_t, uint32_t, int);
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
/* 2 pending, 0 completed, 1 stop (recording failed or uncertain outcome).
 * TOOLS_CAPACITY: result buffer below AGENT_RESULT_CAP; nothing ran, result
 * is empty, and it is not a step stop. Nonzero, so callers that abort on any
 * nonzero return still do. */
#define TOOLS_CAPACITY (-1)
int tools_execute_recorded(const AgentCall *call, char *result, size_t cap,
                           AgentJournal journal, void *context);
/* Common preflight for both dispatch paths. tokens has 128 entries; -1
 * supplies an error result without executing. New mutates rows guard path
 * automatically, or name a different writable argument in the registry. */
int tools_check_arguments(const ToolDef *, const AgentCall *, JsonToken *tokens,
                          char *result, size_t cap);
/* Dispatch with the tokens from a successful preflight; no reparse or second
 * evidence check. The app uses this for immediate tools and tools.c's pending
 * read/edit/copy; other pending executors stay with their owning modules. */
int tools_execute_checked(const AgentCall *, const JsonToken *, char *, size_t,
                          AgentJournal, void *, uint32_t now);
const char *tools_text_phase(void);
int tools_text_step(char *, size_t, uint32_t, int stop);
/* Advance whichever pending tool operation is active: a text read/edit or a
 * copy. The generic host and diagnostic drivers pump this; main.c pairs the
 * registry rows with their own step functions instead. */
int tools_pending_step(char *, size_t, uint32_t, int stop);
int tools_validate_path(const char *path, int folder);
/* Case-insensitive whole-component match of the configured relative queue. */
int tools_path_in_queue(const char *workspace_relative_path);
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
