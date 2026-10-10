/* The tool registry: one row per model-facing tool, the single definition
 * site of its name, wire schema and classification. agent.c joins the
 * schemas into one request array, tools.c lists the names in
 * get_environment and dispatches by id, and main.c pairs pending rows
 * with run states. Pure data on purpose: the host suites that link agent.c
 * alone cannot link the tool executors or the File Manager model, so this
 * file must not grow code dependencies. */
#include "tools.h"
#include <stddef.h>
#include <string.h>

const ToolDef kToolDefs[] = {
    { TOOL_get_environment, "get_environment",
        "{\"type\":\"function\",\"function\":{\"name\":\"get_environment\","
        "\"description\":\"Report the native OS, workspace, encoding and installed tool capabilities.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}}}",
        0, 0 },
    { TOOL_list_files, "list_files",
        "{\"type\":\"function\",\"function\":{\"name\":\"list_files\","
        "\"description\":\"List a workspace folder. Paths are relative classic colon-separated paths; empty root means the workspace. Results include paths usable by read_text. Bounded and paginated.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"root\":{\"type\":\"string\"},"
        "\"cursor\":{\"type\":\"integer\",\"minimum\":0},\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":12}},"
        "\"required\":[\"root\"],\"additionalProperties\":false}}}",
        0, 0 },
    { TOOL_read_text, "read_text",
        "{\"type\":\"function\",\"function\":{\"name\":\"read_text\","
        "\"description\":\"Read bounded plain MacRoman text as UTF-8, with line range and byte continuation. Pages fill the " TOOLS_LIMIT_STRINGIFY(AGENT_RESULT_CAP) "-byte JSON result budget; escaping can reduce text per page. Files up to " TOOLS_FILE_CAP_DESCRIPTION " bytes get a whole-file revision independent of pagination; larger files get observational scan revisions. editable indicates CR text within the edit limit. Refuses binary/resource-fork files. Use paths from list_files.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
        "\"start_byte\":{\"type\":\"integer\",\"minimum\":0},\"start_line\":{\"type\":\"integer\",\"minimum\":1},\"max_lines\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":30}},"
        "\"required\":[\"path\"],\"additionalProperties\":false}}}",
        0, 1 },
    { TOOL_search_text, "search_text",
        "{\"type\":\"function\",\"function\":{\"name\":\"search_text\",\"description\":\"Case-sensitive literal search of plain MacRoman text. Returns paths, absolute line "
        "numbers, byte offsets and excerpts. Bounded to 64 catalog entries and 8192 read bytes per call, recursive depth 8. Pass next_cursor unchanged with the"
        " same root/query/recursive until truncated is false; keep the tree unchanged between pages. Skips aliases, resource forks and binary scan ranges. Read"
        " matches before editing.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"root\":{\"type\":\"string\"},\"query\":{\"type\":\"string\",\"minLength\":1},\"recursive\":{\"t"
        "ype\":\"boolean\"},\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8},\"cursor\":{\"type\":\"string\"}},\"required\":[\"root\",\"query\"],\"additionalProperties\":fals"
        "e}}}",
        0, 0 },
    { TOOL_write_text, "write_text",
        "{\"type\":\"function\",\"function\":{\"name\":\"write_text\","
        "\"description\":\"Create a NEW plain text file in an existing workspace folder. Never overwrites. Relative colon-separated path; strict UTF-8 to MacRoman conversion, CR line endings, Finder type TEXT. Maximum " TOOLS_STRING_CAP_DESCRIPTION " encoded bytes; arguments also bounded to 8192 bytes. Refuses aliases and binary controls. Returns path, bytes and revision; verify with read_text. Never retry an uncertain outcome.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
        "\"text\":{\"type\":\"string\"}},\"required\":[\"path\",\"text\"],\"additionalProperties\":false}}}",
        1, 0 },
    { TOOL_edit_text, "edit_text",
        "{\"type\":\"function\",\"function\":{\"name\":\"edit_text\","
        "\"description\":\"Edit existing plain MacRoman/CR text up to " TOOLS_FILE_CAP_DESCRIPTION " bytes. Read first: expected_revision must be its current whole-file revision. Each replacement string must fit " TOOLS_STRING_CAP_DESCRIPTION " MacRoman bytes. Replace exactly one nonempty old_text match with new_text (empty means deletion). Overlapping/repeated matches, stale revisions, aliases, resource forks, binary controls, unsupported Unicode and oversized results fail. Model LF/CRLF normalize to CR. Stages verified TEXT, retains original at backup_path, journals publication; verify with read_text. Never retry an uncertain outcome; report recovery paths.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
        "\"expected_revision\":{\"type\":\"string\"},\"old_text\":{\"type\":\"string\",\"minLength\":1},"
        "\"new_text\":{\"type\":\"string\"}},\"required\":[\"path\",\"expected_revision\",\"old_text\",\"new_text\"],"
        "\"additionalProperties\":false}}}",
        1, 1 },
    { TOOL_create_folder, "create_folder",
        "{\"type\":\"function\",\"function\":{\"name\":\"create_folder\","
        "\"description\":\"Create ONE new folder in an existing workspace folder. Never reuses an existing name and never creates intermediate folders: create each level in turn. Relative colon-separated path without a trailing colon. Refuses aliases. Journaled; verify with list_files. Never retry an uncertain outcome.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
        "\"required\":[\"path\"],\"additionalProperties\":false}}}",
        1, 0 },
    { TOOL_move_to_trash, "move_to_trash",
        "{\"type\":\"function\",\"function\":{\"name\":\"move_to_trash\","
        "\"description\":\"Move up to 8 workspace files to the Trash with one same-volume rename each; never a permanent delete and never a folder. Every item is {path, revision}: the cat- revision must come from list_files or get_file_info, and a file that changed since is refused. Refuses aliases, locked files, build evidence and contents of a Trash. Journaled; verify with list_files or get_file_info and say where each file went. An uncertain outcome reports both paths and is never retried.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"files\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":8,"
        "\"items\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"revision\":{\"type\":\"string\"}},"
        "\"required\":[\"path\",\"revision\"],\"additionalProperties\":false}}},"
        "\"required\":[\"files\"],\"additionalProperties\":false}}}",
        1, 0 },
    { TOOL_create_project, "create_project",
        "{\"type\":\"function\",\"function\":{\"name\":\"create_project\","
        "\"description\":\"Create a NEW ppc-toolbox-v1 project folder in an existing non-alias workspace parent. Only path is accepted, relative colon-separated without trailing colon. Publishes the verified sources, app.r, tmpl.h and project.json together; source is MacRoman/CR/TEXT. Never reuses existing folders. Read sources before editing. Build with build_project; launch successful artifacts with run_application(build_id). Failed staging is retained at temporary_path; never retry uncertain mutations.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
        "\"required\":[\"path\"],\"additionalProperties\":false}}}",
        1, 0 },
    { TOOL_build_project, "build_project",
        "{\"type\":\"function\",\"function\":{\"name\":\"build_project\",\"description\":\"Compile a workspace project folder using project.json protocol 2 and toolchain mpw-ppc-v2. Descriptor requires protocol:2, toolchain:mpw-ppc-v2, sources:[C paths], output:lowercase filename; optional resources:[Rez paths], headers:[header paths], include_paths:[folder paths or .], settings:{warnings:off,libraries:[InterfaceLib,StdCLib],creator:4 alphanumeric characters}. Paths use lowercase ASCII letters, digits, dash, underscore, dot and colon separators. Up to ten total inputs of " TOOLS_FILE_CAP_DESCRIPTION " bytes each, a " TOOLS_DESCRIPTOR_CAP_DESCRIPTION "-byte descriptor and a " TOOLS_SNAPSHOT_CAP_DESCRIPTION "-byte total snapshot including recipe and manifest. Runs natively through MPW ToolServer. Returns revision-bound snapshot and build ID. Five-minute deadline; Stop ends observation, never cancels/replays. Never retry uncertain builds.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"],\"additionalProperties\":false}}}",
        1, 1 },
    { TOOL_read_build_log, "read_build_log",
        "{\"type\":\"function\",\"function\":{\"name\":\"read_build_log\",\"description\":\"Read retained native build stdout/stderr (128 bytes). Follow next_byte until truncated is false. MacRoman; binary control bytes display as ?.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"build_id\":{\"type\":\"string\"},\"stream\":{\"type\":\"string\",\"enum\":[\"stdout\",\"stderr\"]},\"start_byte\":{\"type\":\"integer\",\"minimum\":0}},\"required\":[\"build_id\",\"stream\"],\"additionalProperties\":false}}}",
        0, 0 },
    { TOOL_run_application, "run_application",
        "{\"type\":\"function\",\"function\":{\"name\":\"run_application\",\"description\":\"Launch only an authorized successful build_id. Verifies artifact identity and both forks against the persisted build record. Returns separate run_id, snapshot, native process observation and quit_supported, not a smoke-test pass. A pre-existing app gets no new quit authority; original_run_id retains its owned close handle when available. Stop interrupts verification before launch. Never retry uncertain launches.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"build_id\":{\"type\":\"string\"}},\"required\":[\"build_id\"],\"additionalProperties\":false}}}",
        1, 1 },
    { TOOL_quit_application, "quit_application",
        "{\"type\":\"function\",\"function\":{\"name\":\"quit_application\",\"description\":\"Request graceful noninteractive Quit only for an owned run_id with quit_supported:true from this Sherclawk process. Never accepts arbitrary paths or PSNs. New Chat preserves ownership; restart loses it. No force quit or discard changes. Observes exit for 30 seconds; acceptance alone is not success. Stop after send cannot cancel. A send attempt cannot be repeated through that handle, including refusal or uncertainty. Already-exited owned apps return ALREADY_EXITED without sending. Never clean up apps automatically.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"run_id\":{\"type\":\"string\"}},\"required\":[\"run_id\"],\"additionalProperties\":false}}}",
        1, 1 },
    { TOOL_get_file_info, "get_file_info",
        "{\"type\":\"function\",\"function\":{\"name\":\"get_file_info\","
        "\"description\":\"Read-only Finder catalog identity for one workspace file or folder: kind, four-character type and creator, flags (alias, custom_icon, bundle, invisible, locked), label, data/resource fork sizes and catalog created/modified dates formatted as YYYY-MM-DD HH:MM:SS. Does not change the file.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
        "\"required\":[\"path\"],\"additionalProperties\":false}}}",
        0, 0 },
    { TOOL_resolve_alias, "resolve_alias",
        "{\"type\":\"function\",\"function\":{\"name\":\"resolve_alias\","
        "\"description\":\"Read-only resolution of an HFS alias file to its target: leaf name, kind, existence, whether the alias record was updated, and the workspace-relative target path when a bounded catalog walk finds it. relative_path is null for outside or not-found targets; outside_workspace may be null when the 512-entry walk was incomplete. Refuses non-alias files.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
        "\"required\":[\"path\"],\"additionalProperties\":false}}}",
        0, 0 },
    { TOOL_list_processes, "list_processes",
        "{\"type\":\"function\",\"function\":{\"name\":\"list_processes\","
        "\"description\":\"Read-only Process Manager listing: name, high:low ProcessSerialNumber, and front/self flags per process. Paginated; pass next_cursor unchanged until truncated is false. Cooperative liveness evidence, not a launch or quit capability.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"cursor\":{\"type\":\"string\"},"
        "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":12}},\"additionalProperties\":false}}}",
        0, 0 },
    { TOOL_list_fonts, "list_fonts",
        "{\"type\":\"function\",\"function\":{\"name\":\"list_fonts\","
        "\"description\":\"Read-only list of installed font families from the Mac OS 9 Font Manager: family id and name. Paginated; pass next_cursor unchanged until truncated is false. Use a returned id (or name) with measure_text.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"cursor\":{\"type\":\"integer\",\"minimum\":0},"
        "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":24}},\"additionalProperties\":false}}}",
        0, 0 },
    { TOOL_measure_text, "measure_text",
        "{\"type\":\"function\",\"function\":{\"name\":\"measure_text\","
        "\"description\":\"Read-only QuickDraw measurement of one printable MacRoman line (at most 256 encoded bytes; LF/CR/controls refused). Provide exactly one of font (installed name) or font_id (from list_fonts), optional size 1-127 and style (comma-separated bold, italic, underline, outline, shadow, condense, extend; default plain). Returns the width in pixels plus ascent, descent, leading and line height for classic layout checks.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"minLength\":1},"
        "\"font\":{\"type\":\"string\"},\"font_id\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":32767},"
        "\"size\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":127},\"style\":{\"type\":\"string\"}},"
        "\"required\":[\"text\"],\"additionalProperties\":false}}}",
        0, 0 },
    { TOOL_list_resources, "list_resources",
        "{\"type\":\"function\",\"function\":{\"name\":\"list_resources\","
        "\"description\":\"Read-only listing of a workspace file's resource fork: type, id, byte size and name per resource, paginated with a type:resource cursor. Use it to verify what a build actually produced and to inspect existing applications. Refuses aliases, folders and files without resource forks; never edits resources.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
        "\"cursor\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":16}},"
        "\"required\":[\"path\"],\"additionalProperties\":false}}}",
        0, 0 },
    { TOOL_read_resource, "read_resource",
        "{\"type\":\"function\",\"function\":{\"name\":\"read_resource\","
        "\"description\":\"Read bounded bytes of one resource (type plus signed id) from a workspace file's resource fork. 'TEXT' and 'STR ' return MacRoman text (a string's length prefix is excluded); 'vers' decodes version, stage and short/long strings; everything else returns uppercase hex. Follow next_byte until truncated is false. Read-only evidence; resources never authorize an edit or launch.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
        "\"type\":{\"type\":\"string\"},\"id\":{\"type\":\"integer\",\"minimum\":-32768,\"maximum\":32767},"
        "\"start_byte\":{\"type\":\"integer\",\"minimum\":0},\"max_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":256}},"
        "\"required\":[\"path\",\"type\",\"id\"],\"additionalProperties\":false}}}",
        0, 0 },
    { TOOL_view_image, "view_image",
        "{\"type\":\"function\",\"function\":{\"name\":\"view_image\","
        "\"description\":\"Read-only: attach one workspace PNG, such as a screenshot a generated application wrote, so you can look at it. Relative classic colon-separated path from list_files. PNG only, at most " TOOLS_LIMIT_STRINGIFY(AGENT_IMAGE_CAP) " bytes; one image per round. The pixels arrive in a user message right after the tool results and only in the next request, so describe what matters then; call again to see the image again. Fails with an explicit error when the selected model is not known to accept images.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
        "\"required\":[\"path\"],\"additionalProperties\":false}}}",
        0, 1 },
};

#define TOOL_COUNT (sizeof kToolDefs / sizeof *kToolDefs)

const ToolDef *tools_at(size_t i)
{
    return i < TOOL_COUNT ? &kToolDefs[i] : NULL;
}

size_t tools_count(void)
{
    return TOOL_COUNT;
}

const ToolDef *tools_lookup(const char *name)
{
    size_t i;
    if (!name) return NULL;
    for (i = 0; i < TOOL_COUNT; i++)
        if (!strcmp(kToolDefs[i].name, name)) return &kToolDefs[i];
    return NULL;
}
