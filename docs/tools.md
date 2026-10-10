# File and inspection tools

These are the native model-facing contracts. For project creation, compiler
execution and artifact launch, see [native builds](native-builds.md). Shared
buffer and deadline relationships are in [limits](limits.md).

| Capability | Tools |
|---|---|
| Environment, catalog and text reads | `get_environment`, `list_files`, `read_text` |
| Search and source changes | `search_text`, `write_text`, `create_folder`, `move_to_trash`, `edit_text` |
| Native project work | `create_project`, `build_project`, `read_build_log`, `run_application` |
| Read-only platform inspection | `get_file_info`, `resolve_alias`, `list_processes`, `list_fonts`, `measure_text`, `list_resources`, `read_resource` |
| Read-only image viewing | `view_image` |

## Workspace paths and text

The default workspace is `Retro68:`. Configure a native MacRoman path ending
in a colon. Tool paths are relative to it, for example `Spikes:ccapp:hello.c`;
leading colons, parent traversal, slash paths, and invalid names are refused.
Workspace source text is interpreted as MacRoman, converted to UTF-8 for the
model, and displayed through native TextEdit. Text tools refuse binary files,
resource forks and aliases; the separate inspection tools can report metadata
and resource contents as specified below. Tool arguments never pass through
lossy UI conversion.

`AGENTS.md` files are not read through a tool: the app loads them under the
same plain-text rules (see [project instructions](usage.md#project-instructions-agentsmd)).
The model may update an existing one with `edit_text` like any other text file.

Tool results fit a fixed JSON envelope below 4,096 bytes. Folder listings have
cursors; `list_files` and `get_file_info` report a catalog revision (`cat-…`:
catalog identity, modification date and both fork sizes) for files.
`move_to_trash` requires that revision; it is not a content hash and `edit_text`
never accepts it. Text reads supply `next_byte` when a line is partial and scan
at most 8 KiB per invocation. Continue from the returned cursor or byte offset
rather than assuming the first page is complete. Small-file whole-file revisions
can guard edits; larger-file scan revisions are observational. See
[editing text](#editing-text) for the exact guard requirements.

## Protected MCP configuration

`System Folder:Preferences:Sherclawk MCP Servers.json` and the
`….json.new` and `….json.old` files a save stages beside it hold MCP
credentials in clear text. No model-facing tool reads, lists, searches,
creates, replaces, edits or trashes them, even when the configured workspace
contains the Preferences folder. See [MCP](mcp.md#configuration-protection).

- **Identity, not spelling.** The guard names the files by the Preferences
  folder's volume and directory ID (from `FindFolder`) plus the leaf name,
  compared without case. It runs where a path becomes a catalog entry
  (`tools_resolve`), so `read_text`, `get_file_info`, `list_resources`,
  `read_resource`, `view_image`, `write_text`, `edit_text`, `create_folder`,
  `create_project`, `move_to_trash`, `build_project` inputs and `run_application`
  all share it. An absent protected name is refused too, so a tool cannot plant
  a configuration.
- **Enumeration.** `list_files` omits protected entries and its cursor moves
  past them. `search_text` skips them (counted in `skipped`) in flat and
  recursive searches and never reads their bytes.
- **Aliases.** `resolve_alias` checks the resolved target by identity and
  refuses a protected one without reporting its name or location.
- **Failure.** A refusal is `{"status":"error","code":"PROTECTED",…,"os_error":30001}`
  with fixed text that names no path or content; `30001` is not a File Manager
  code. If the Preferences folder cannot be located, every guarded call fails the
  same way, because nothing can then be shown to be unprotected.
- **Everything else is unchanged.** Other files in the Preferences folder, and a
  workspace file that merely has the same name elsewhere, are ordinary.

## Searching text

Text tools v2 adds `search_text(root, query, recursive=false, limit=4, cursor)`.
Use an empty root for the workspace or a relative colon-separated folder.
Omit the cursor or pass an empty string to start a new search.
Search is case-sensitive and literal; queries must be single-line MacRoman
text, 1–128 encoded bytes. Matches include paths, absolute one-based line
numbers, zero-based data-fork byte offsets and short UTF-8 excerpts starting
at the match. Read the source before editing; search does not supply revisions.
CR, LF and CRLF line endings are counted, including across pages.

Each invocation reads at most 8 KiB and examines at most 64 catalog entries
plus at most eight ancestor entries to restore recursive traversal. Recursive
search goes up to eight folders below the root. Results contain at most eight
matches and fit the existing tool-result bound. Matches spanning scan boundaries
are included; overlapping matches are returned separately. Empty pages can
still have `truncated: true`: pass `next_cursor` unchanged with the same root,
query and recursion setting until `truncated: false` and `next_cursor: null`.
Stop prevents the next bounded call; it does not interrupt a File Manager call.

Cursors are observational catalog positions, not snapshots. Keep the tree and
file contents unchanged between pages, and restart search after an edit.
Aliases (including root ancestors), resource forks and unsupported file types
are skipped or refused. Binary controls invalidate the current scan range;
earlier pages are not whole-file binary certification. `skipped` reports excluded
entries/ranges, including folders beyond the depth/path bounds. A folder with
more than 30,000 entries requires a narrower search root. I/O and detected
within-scan catalog changes return errors instead of claiming completion.

## Creating text

`write_text(path, text)` creates a new plain data-fork file in an existing
workspace folder. It never overwrites or creates parent folders. Paths use the
same relative colon syntax as reads; each parent is resolved by directory ID,
and aliases are refused. Text must convert strictly from UTF-8 to MacRoman;
unrepresentable characters and binary controls fail before creating a file.
LF and CRLF normalize to classic CR. Files have Finder type `TEXT`, creator
`ttxt`, and no resource fork, so native MPW tools can consume the source.

The encoded file limit is 4,096 bytes, also subject to the existing 8 KiB JSON
argument limit. Sherclawk records a mutation intent, writes a uniquely named
sibling temporary file, closes and flushes it, verifies its bytes and metadata,
and records the staged file before publishing by a non-overwriting rename.
It flushes the volume and records completion before returning `CREATED`, the
path, byte count and revision. Use `read_text` to verify the contents; new files
fit within one read scan, although displayed results still require pagination.

Mutations require a durable journal. Recording failures or uncertain publication
stop the run; no automatic mutation retry occurs. Recovery records carry the
call ID, destination, temporary path, byte count and revision. Failed stages
are preserved for inspection, and a crash between rename and completion can
leave the destination present with only a staged record. Inspect both paths
before taking further action. Automatic recovery is not implemented. These
small synchronous writes finish before another UI event is handled; Stop
prevents subsequent calls and does not undo a completed create.

## Creating folders

`create_folder(path)` creates one new folder in an existing workspace folder,
using the same relative colon syntax and alias refusal as `write_text`. It never
reuses an existing name and never creates intermediate levels: create each level
in turn. A trailing colon, empty path or missing parent fails before the journal
is touched. Sherclawk records a mutation intent, creates the folder (HFS
creation is atomic, so there is no staging step), flushes the volume, verifies
the catalog entry is a non-alias folder, and records completion before
returning `CREATED_FOLDER`. A failure that may have left a folder, an
unverifiable result or a failed completion record is reported as `uncertain`
and stops the run. Use `list_files` to verify; Stop does not undo a create.

## Moving files to the Trash

`move_to_trash(files)` moves 1–8 files into the volume's Trash with one
same-volume rename each. It never permanently deletes, never moves folders and
never empties a Trash. Every item is exactly `{path, revision}`: the `cat-…`
catalog revision from `list_files` or `get_file_info` pins catalog identity,
modification date and both fork sizes, and a target that changed since it was
listed is refused before any rename. The whole batch is validated first; if any
item fails, nothing moves.

The destination is resolved through the Folder Manager for the source's volume.
On an AppleShare volume that is the client's `Network Trash Folder`, so items
appear in the guest Trash and Empty Trash clears them as usual. When no
same-volume Trash resolves, the tool journals and creates `Retro68:Sherclawk
Trash:` once and says so in the result; it never copies across volumes. The
move keeps the file's name, so a name already in that Trash is refused `EXISTS`
before any rename.

Each item journals `mutation_intent`, renames, verifies the destination catalog
identity (both fork sizes, type and creator) and source absence, then journals
`mutation_committed`. A rename error counts as a plain failure only while the
same file is still at the source path; otherwise the reply may have been lost
and verification decides. A journal failure before the rename stops the run
with that file untouched; a failure after it, or failed verification, is
`uncertain`, stops the run and reports both paths. Refused without a journal
record: folders, aliases, locked files, anything under `Worker01:buildjobs`
(launched-build evidence), anything already inside a Trash, a non-`cat-`
revision, and a batch whose report cannot fit the 4,096-byte result cap.
If the report itself cannot be formatted after files moved, the result is
`uncertain` `REPORT_LIMIT` with only the `moved` count; inspect the Trash before
retrying. Recover an item by moving it back from the Trash under its reported `moved_as`
name, or empty the Trash as usual; Stop does not undo a completed rename.

## Inspecting resources and identity

Seven read-only tools report what the workspace and the running system
actually contain; none of them writes to a volume, changes Finder state or
authorizes execution. Results use the same bounds and pagination conventions
as the other tools.

`get_file_info(path)` reports kind, four-character type and creator, the raw
Finder flags plus named alias/custom-icon/bundle/invisible/locked booleans,
the label, both fork sizes and created/modified catalog dates formatted as
`YYYY-MM-DD HH:MM:SS` (converted locally, not through International
Utilities). Files also report a `cat-…` catalog revision for
`move_to_trash`; folders omit the file-only fields.

`resolve_alias(path)` reads an HFS alias file's `alis`/0 resource into a
bounded private copy (at most 32 KiB), resolves it, and
reports the target's leaf name, kind, existence and whether the record was
updated. A bounded catalog walk (512 entries, depth 8) finds the target's
parent so an inside-workspace `relative_path` can be returned;
`relative_path` is null for outside targets, and `outside_workspace` is
true, false or null when the walk was incomplete. Non-alias files are
refused with `NOT_ALIAS`.

`list_processes(cursor, limit)` pages the native Process Manager with each
name, `high:low` ProcessSerialNumber and front/self flags. `list_fonts`
pages installed families as family id and name from the Mac OS 9 Font
Manager. `measure_text(text, font or font_id, size, style)` measures one
printable MacRoman line (controls refused) while saving and restoring the
current port's font state; bold, italic, underline, outline, shadow,
condense and extend combine comma-separated. It returns the pixel width and
ascent/descent/leading/line height for classic layout checks. A name and an
id may be supplied together when they resolve to the same family; an empty
name string counts as absent.

`list_resources(path, cursor, limit)` pages a file's resource map with type,
signed id, byte size and name; the cursor is a returned `type:resource`
pair. `read_resource(path, type, id, start_byte, max_bytes)` reads at most
256 bytes per call: `TEXT` and `STR ` decode to MacRoman text (a string's
one-byte Pascal length prefix supports 0–255 bytes and is excluded from the
page cursor; missing or overstated prefixes are refused), `vers` decodes to version,
stage, prerelease, region and short/long strings (resources over 256 bytes
are refused). Versions include the bug-fix component when nonzero, and stages
use the classic development/alpha/beta/release constants. Every other type
returns uppercase hex. Handles open with
automatic loading disabled: sizes come from the map and bytes arrive through
`ReadPartialResource`, so a resource fork is never loaded whole. Aliases,
folders, files without resource forks and types that are not four printable
characters are refused. A file that changes during a listing or read is
reported as `CHANGED` instead of returning mixed evidence; cursors are
observational catalog positions.

## Viewing images

`view_image(path)` lets a vision-capable model look at one PNG in the
workspace, for example a screenshot a generated application wrote. Like the
other inspection tools it changes nothing and writes no journal record of its
own.

The pixels are not in the tool result, which stays small. Chat-completions
tool messages cannot carry images, so after the last tool result of the round
the agent records one user message, a short text note naming the path,
dimensions and byte size, and the next model request replaces that note with
the same text plus the image as a base64 `data:image/png` URL. Only that one
request carries the pixels: the history, the session journal and any handoff
request keep the note alone, so the model must call `view_image` again to look
a second time. The file stays in the workspace untouched.

The bytes are sent as they are on disk. Sherclawk checks the PNG signature,
the `IHDR` chunk (each dimension 1 to 8192 pixels) and the `IEND` trailer but
never decodes, converts or downscales, so a generated application must render
a screenshot small enough to fit. The file must be a plain data-fork file of at
most `AGENT_IMAGE_CAP` bytes (see [limits](limits.md)). It is read in slices of
16 KiB per event-loop turn; the file is re-checked each slice and a change
mid-read is reported as `CHANGED`. Stop ends the read and attaches nothing.

The tool is always advertised, but it fails closed. The selected model's
OpenRouter catalog row lists its `architecture.input_modalities`; only a row
that lists `image` allows the tool to read anything. A row that does not is
`VISION_UNSUPPORTED`, and a model whose row could not be fetched or has no such
list is `VISION_UNKNOWN`. Both results tell the model it has not seen the
picture, so it must not describe one.

| Code | Meaning |
|---|---|
| `ARGUMENTS` | Not exactly one `path` string. |
| `VISION_UNSUPPORTED` / `VISION_UNKNOWN` | The model does not, or is not known to, accept images. |
| `FILE` | The workspace path does not resolve. |
| `NOT_PNG` | A folder, alias, too-short file, missing signature or `IHDR`, dimensions out of range, or no `IEND` trailer (truncated or still being written). |
| `TOO_LARGE` | Over `AGENT_IMAGE_CAP` bytes. |
| `ONE_IMAGE_PER_ROUND` | A second `view_image` in the same round. |
| `READ`, `CHANGED`, `TIMEOUT`, `STOPPED` | The read failed, the file changed, took over a minute, or was stopped. |

If the conversation is already so full that the encoded image could not fit in
one request, the image is not attached and the note says so explicitly.

## Editing text

`edit_text(path, expected_revision, old_text, new_text)` replaces exactly one
nonempty match in an existing plain MacRoman/CR file. Read the file first and
use its current `full-...` revision. Repeated or overlapping matches, stale
revisions, missing matches and unchanged replacements fail before staging.
An empty `new_text` deletes the match. UTF-8 arguments convert strictly to
MacRoman, with LF/CRLF normalized to CR; binary controls are refused. Both
the source and edited result must fit the selected file limit (4 KiB shipping;
64 KiB in acceptance builds). Each replacement string remains at most 4,096
MacRoman bytes. Existing LF/CRLF files
are refused rather than silently converting unrelated source bytes.

Reads of files up to the selected file limit return a whole-file revision based on catalog
identity, modification time, byte count and a hash of every byte, independent
of the displayed line range or byte page. `revision_scope` identifies this
as `whole_file`; `editable` identifies CR text within the edit limit. Larger
reads return `scan-...` observational tokens, which editing never accepts.
The catalog `cat-…` revisions from `list_files`/`get_file_info` are only for
`move_to_trash`; `edit_text` requires a `full-…` read revision.
The snapshot is read and validated in 1 KiB chunks, then reread and compared
byte for byte with catalog checks and a successful close. Line navigation spans
the complete snapshot. Revision hashes detect ordinary changes; they are not
cryptographic signatures. Read and edit are pending tools with 60-second
deadlines; transfer, verification and matching work is cooperative. Stop before
the first rename leaves the original in place and reports any retained stage;
from the first rename attempt onward, interruption is uncertain.

Editing refuses aliases (including parents), folders, resource forks and
binary files. It holds an exclusive File Manager read/write open on the
original, stages and verifies new Finder `TEXT`/`ttxt` data, and journals
`mutation_intent` and `mutation_staged` before moving the original to a unique
sibling `Sherclawk bak ...` name. It flushes and verifies that backup's identity,
metadata and exact bytes, records `mutation_backed_up`, then publishes by a
collision-safe rename, verifies the published bytes, closes the original and
records `mutation_committed`. A successful `EDITED` result includes the new
revision, previous revision and `backup_path`. The backup preserves the original
file and Finder metadata and remains for manual inspection. Verify with
`read_text`; backups are never automatically deleted.
If the destination and both recovery paths cannot fit a bounded tool result,
the edit is refused before staging.

The two renames are not an atomic transaction: a crash or failure after moving
the original can leave the destination absent, with backup and staging paths
in the journal/result. Any uncertain publication or journal failure stops the
run; inspect all paths before another mutation. There is no automatic rollback,
recovery or retry. File Manager sharing rules do not protect against direct
POSIX writes on the AFP server, so do not modify the same source that way during
an edit. Stop prevents subsequent calls; it does not interrupt or undo a small
synchronous edit already executing.

## Quit an owned application

`quit_application({"run_id":"…"})` is guest-verified and advertised in
`agent_tool_schemas` and `get_environment`. It accepts exactly one run ID from a launch Sherclawk confirmed and journaled
in this process. No path, arbitrary PSN, force quit, or discard-changes option
is accepted. New Chat keeps ownership; quitting Sherclawk loses it, and old
journals never restore it. Chat close and app exit perform no automatic cleanup.

Successful launch results include `quit_supported`, `quit_reason`, and
`original_run_id`. A pre-existing application gets no new authority; when it
was already owned, the original run ID remains its only close handle. A failed
or incomplete snapshot, unavailable dispatcher, or full ownership registry
preserves launch behavior with `quit_supported:false` and a reason.

Before sending, Quit verifies the exact PSN, artifact FSSpec, launch date and
launcher, rejecting Sherclawk, Finder and system processes. `ALREADY_EXITED`
means absence was observed without sending. `QUIT_OBSERVED` means the owned
process disappeared. A queued noninteractive Quit requests normal cleanup;
a successful reply alone is insufficient. An explicit refusal while still
present returns `error/QUIT_REFUSED` with `native_error`. Timeout after 30
seconds, malformed replies, observation failures, ambiguous send errors and
post-send journal failures return `uncertain`. Stop before send prevents Quit;
after send it stops observation and cannot withdraw the request.

`quit_intent` is journaled before sending, followed by `quit_submitted` and
terminal `quit_observed`. A failed intent journal prevents sending. Once a
send is attempted, its run ID cannot send another Quit, including after an
uncertain outcome or refusal. Late and wrong-sender replies cannot advance
later operations. A tool result of `uncertain` must be reported without retry.

Text read pages use the actual UTF-8/JSON envelope size, bounded to 4096 bytes,
rather than a fixed raw-byte allowance. The default is 20 lines (maximum 30).
Escaping and MacRoman conversion reduce page payloads. Use `next_byte` for
partial lines; CRLF pairs are never divided between pages.

The larger-file workflow remains guest-gated: see [its design and acceptance
requirements](large-text.md). Shipping schemas and `get_environment` report the
verified 4 KiB limit until that gate passes. Environment also reports
`edit_string_max_bytes`, `build_input_max_bytes`, `descriptor_max_bytes` and
`total_snapshot_max_bytes`; replacement strings stay 4 KiB and the descriptor
and aggregate snapshot remain 4 KiB and 128 KiB respectively.
