# File and inspection tools

These are the native model-facing contracts. For project creation, compiler
execution and artifact launch, see [native builds](native-builds.md). Shared
buffer and deadline relationships are in [limits](limits.md).

| Capability | Tools |
|---|---|
| Environment, catalog and text reads | `get_environment`, `list_files`, `read_text` |
| Search and source changes | `search_text`, `write_text`, `create_folder`, `edit_text` |
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

Tool results fit a fixed JSON envelope below 1,536 bytes. Folder listings have
cursors; text reads supply `next_byte` when a line is partial and scan at most
8 KiB per invocation. Continue from the returned cursor or byte offset rather
than assuming the first page is complete. Small-file whole-file revisions can
guard edits; larger-file scan revisions are observational. See
[editing text](#editing-text) for the exact guard requirements.

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

## Inspecting resources and identity

Seven read-only tools report what the workspace and the running system
actually contain; none of them writes to a volume, changes Finder state or
authorizes execution. Results use the same bounds and pagination conventions
as the other tools.

`get_file_info(path)` reports kind, four-character type and creator, the raw
Finder flags plus named alias/custom-icon/bundle/invisible/locked booleans,
the label, both fork sizes and created/modified catalog dates formatted as
`YYYY-MM-DD HH:MM:SS` (converted locally, not through International
Utilities). Folders omit the file-only fields.

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
the source and edited result must fit 4,096 bytes. Existing LF/CRLF files
are refused rather than silently converting unrelated source bytes.

Reads of files up to 4 KiB return a whole-file revision based on catalog
identity, modification time, byte count and a hash of every byte, independent
of the displayed line range or byte page. `revision_scope` identifies this
as `whole_file`; `editable` identifies CR text within the edit limit. Larger
reads return `scan-...` observational tokens, which editing never accepts.
Revision hashes detect ordinary changes; they are not cryptographic signatures.

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
