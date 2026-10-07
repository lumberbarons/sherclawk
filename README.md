# Sherclawk — The consulting crustacean

A native, sequential agent loop for classic Mac OS 9 over direct OpenRouter
HTTPS, with a Finder icon generated from `art/sherclawk.png` and that character
in the main window. It began as an independent source copy of HelloChat and now
lives in its own repository. [PLAN.md](PLAN.md) tracks the full coding-harness
roadmap.

The installed tools are `get_environment`, `list_files`, `read_text`,
`search_text`, create-only `write_text`, create-only `create_folder`,
template-backed `create_project`, revision-guarded `edit_text`,
`build_project`, `read_build_log`, `run_application`, and the read-only
inspection set `get_file_info`, `resolve_alias`, `list_processes`,
`list_fonts`, `measure_text`, `list_resources` and `read_resource`
(see "Inspecting resources and identity"). Builds self-execute through
asynchronous ToolServer commands when the queue is unowned.
The MacRelix worker remains an exclusive-owner fallback. Authorized artifacts launch through the native Process Manager. The [native PowerPC template](templates/ppc-toolbox/README.md)
captures a guest-verified MrC/PPCLink/Rez recipe and installed versions;
the [MacRelix job worker](worker/README.md) now implements complete-file
publication, rename claims, output capture and completion records. The native
producer and bounded poller now have a cooperative guest diagnostic;
revision-bound builds are implemented. The [contract](PLAN.md#buildrun-contract)
uses editable project descriptors, multiple source/resource files and structured
toolchain settings; the verified template supplies a convenient starting point.
The model receives only these installed capabilities; the app executes tools
with the File Manager and records their results before requesting a follow-up.

## Build and publish

Prerequisites: Docker with the `ghcr.io/autc04/retro68` image, a Certainly
clone next to this repository, Apple's Universal Interfaces fetched once, and
the vendored `patches/` + `art/` in this tree. No additional imaging or runtime
libraries are required. The stdlib-only artwork converter reads
`art/sherclawk.png` (8-bit RGBA, noninterlaced); override its path with
`SHERCLAWK_ART`.

```bash
git clone --recursive --depth 1 https://github.com/minorbug/certainly.git ../Certainly
tools/get-universal-interfaces.sh
cp config.example.h config.local.h
# Set SHERCLAWK_API_KEY and optionally SHERCLAWK_MODEL / SHERCLAWK_WORKSPACE.
./build.sh
SHARE_HOST=<afp-server> tools/deploy-to-share.sh
```

`config.local.h` is ignored, and credentials are embedded only in local
binaries. Builds without credentials still launch. Quit a running copy before
publishing/relaunching. Normal runtime uses no host executor or model relay.
Model requests identify the client as `Sherclawk/1.0 (Certainly; Mac OS 9)`;
Sherclawk's build defines `SHERCLAWK_APP`, which selects that User-Agent in the
vendored POST builder.
Set `SHERCLAWK_APP_URL` in `config.local.h` to a public app URL to also send
OpenRouter's `HTTP-Referer` and `X-OpenRouter-Title: Sherclawk` attribution
headers; an empty value sends neither.

The default workspace is `Retro68:`. Configure a native MacRoman path ending
in a colon. Tool paths are relative to it, for example `Spikes:ccapp:hello.c`;
leading colons, parent traversal, slash paths, and invalid names are refused.
Workspace source text is interpreted as MacRoman, converted to UTF-8 for the
model, and displayed through native TextEdit. Binary/resource-fork files and
aliases are refused. Tool arguments never pass through lossy UI conversion.

The Finder icon ships as a bundle icon family and as an attached custom icon,
so the AFP file displays correctly without rebuilding the Desktop database.
Both 16px and 32px icons have color, masks and monochrome fallback. The native
window draws a 156px RGB resource generated from the same image. Fork-aware
publication keeps the exact netatalk filler/table layout, with Sherclawk's
creator and custom-icon/bundle flags in Finder info.

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

## Creating projects

`create_project` is an optional starter shortcut. `build_project(path)` accepts a project folder path with or without the
trailing colon returned by `list_files`, and any valid `project.json`, including one assembled with ordinary folder/text tools.
`run_application(build_id)` launches the recorded artifact of a successful build.

`create_project(path)` creates a new `ppc-toolbox-v1` project in an existing
workspace parent. Only `path` is accepted, with relative colon syntax and no
trailing colon. Existing destinations, missing parents and aliases are refused.
It publishes `main.c`, `app.r` and a protocol-2 `project.json` as MacRoman/CR
Finder `TEXT`/`ttxt` data forks. The default output is `template`; edit the
descriptor to add sources or change the output. The folder name does not change
the window title. Projects contain no shell recipes.

### Native project builds

The descriptor is editable JSON with this initial supported contract:

```json
{
  "protocol": 2,
  "toolchain": "mpw-ppc-v2",
  "sources": ["main.c", "extra.c"],
  "resources": ["app.r"],
  "headers": ["shared.h"],
  "include_paths": ["."],
  "output": "example",
  "settings": {
    "warnings": "off",
    "libraries": ["InterfaceLib", "StdCLib"],
    "creator": "ShCk"
  }
}
```

Required fields are `protocol`, `toolchain`, nonempty `sources`, and `output`.
Optional `resources`, `headers`, and `include_paths` default to empty arrays.
Settings default to the shown values. `InterfaceLib` is required; `StdCLib` may
be omitted. The verified C runtimes are always linked. No free-form flags are
accepted; unknown/duplicate fields, settings and libraries fail validation.
Optional `template` is provenance only. Protocol-1 descriptors are refused:
replace their combined sources list with separate C sources/Rez resources, add
`toolchain` and `output`, and set `protocol` to 2 through ordinary guarded edits.

Up to five total declared C/Rez/header files and a 4 KiB descriptor are supported;
each input is at most 4 KiB, plain Finder `TEXT`, MacRoman/CR, without resource
forks or aliases. Declare all project-owned headers. Relative paths use colon
separators and lowercase ASCII letters, digits, underscore, dash and dot;
components are 1–31 bytes, cannot start with dot/dash, and paths are at most
95 bytes. Output is one such filename; `success.txt` is reserved. Include paths
allow up to three directories or `.`. Only declared inputs are copied into the
fresh build working directory. Standard SDK includes are supplied by the adapter.

Prepare the queue once on the mounted AFP volume. Sherclawk now acquires the
existing `worker-lock` and executes its current snapshot itself when unowned.
ToolServer and its installed SDK are still required; MacRelix is unnecessary
for this path. Queue creation and general queue service belong to increment 4:

```bash
ssh "$SHARE_HOST" 'sudo -n install -d -o macos9 -g macos9 -m 775 /srv/retro68/Worker01/buildjobs'
```

For the existing fallback, start the worker before publishing builds:

```sh
perl -w /Volumes/Retro68/Worker01/worker05.pl /Volumes/Retro68/Worker01/buildjobs
```

An external lock makes Sherclawk poll without claiming or executing. Locks are
not liveness evidence and are never stolen automatically. Quit the external
worker normally before switching executors; inspect orphaned claims and locks
before manual recovery. An idle background MacRelix worker may require activation.

`build_project` reads/compares closed input pages before reserving a fresh build
ID. Its snapshot retains the descriptor, generated trusted recipe and manifest
with whole-file revisions and recipe hash. Snapshot publication and polling
service events between bounded steps. Five-minute snapshot/job phase deadlines and Stop abandon
observation; they never prove cancellation or replay a published job. An
uncertain result stops the agent run. Keep the queue/journals for inspection.
Revision hashes are observational FNV tokens, not cryptographic attestations;
external server writes are outside File Manager locking guarantees.

Success requires a matching worker success, the exact artifact marker and a
non-alias `APPL` with data and resource forks. Failed/uncertain builds return no
artifact. Later source edits do not change older snapshots. Initial compiler
diagnostics are bounded; `read_build_log(build_id, stream, start_byte)` reads
128-byte retained stdout/stderr pages with `next_byte` and `truncated` continuation.
Log data is displayed as MacRoman text; binary control bytes display as `?` while
raw logs remain on disk. Successful builds persist a private Finder `ShAR`/`ShCk` `launch.rec` in their
snapshot folder after recording the successful build. This versioned native
binary record seals the output name, file identity, modification date, creator,
sizes and FNV hashes of both forks. Old builds without this record require a
fresh build; IDs or artifact paths alone cannot authorize execution. The worker
queue and its snapshots are read-only to model-facing source mutation tools.

### ToolServer diagnostic (idea 005, increment 1)

`SherclawkToolServerCheck` is a standalone asynchronous Apple-event spike.
The main app still uses the MacRelix worker. Publish the diagnostic with:

```bash
APP=SherclawkToolServerCheck tools/deploy-to-share.sh
```

Run only with ToolServer idle and no worker build in progress. It finds a running
`MPSX` process or discovers and launches ToolServer through mounted volumes'
desktop databases. It creates a fresh `Retro68:ToolServerCheck<ticks>:` fixture
with native `TEXT`/CR Rez sources and writes
`Retro68:SherclawkToolServerCheck.log` (overwritten at each launch). Save that
log before another launch; fixture folders and resource outputs are retained.

The automatic suite sends successful and deliberately failing Rez commands via
`misc/dosc`, `typeChar`, `kAEQueueReply | kAENeverInteract`. It verifies the
successful `STR `/128 resource, logs raw `stat`, stdout and diagnostics, and
checks wrong return IDs/senders, missing status, wrong text type and oversized
text using synthetic local events through the actual reply handler. The log's
`RESULT failures=0` applies to this suite; interactive fault tests have separate
records. Raw MacRoman/CR reply bytes remain in the log.

| Key | Diagnostic action |
|---|---|
| `R` | Explicitly start a fresh success/error suite when no command is outstanding |
| `L` | Send one script containing 41 fixed Rez invocations for interaction tests |
| `S` or Command-period | Stop observing; retain and drain the outstanding reply, keeping outcome unknown |
| `T` | Expire the outstanding command's deadline; drain its late reply without advancing |
| `K` | Politely request ToolServer quit (it may defer until the command finishes) |
| Command-Q | Quit the diagnostic, recording any outstanding command as unknown |

There is one outstanding command at a time and no automatic resend. If
ToolServer disappears, the request is retired as unknown; `R` may then test a
fresh launch. A reply whose sender cannot be verified is ignored. A lost reply
from a still-running server keeps fresh tests disabled until diagnostic exit.
Replies are capped at 8 KiB per text parameter for extraction; this does not cap
the Apple Event Manager's allocation of the incoming event itself. Logs include
send duration, elapsed ticks, event-loop turns, updates and maximum turn gap.
The window can be dragged, but ordinary Toolbox `DragWindow` tracking can defer
reply handling; production code must account for this before promising latency.
This spike does not build C projects or claim queue jobs.

### Integrated self-builds (idea 005, increment 3)

`build_project` publishes the same protocol-1 snapshot, then attempts exclusive
ownership once. `selfbuild.c` renames `ready` to `claimed`, retains the unchanged
`started` record plus `native-executor`, and compares every staged input against
the published representation in 1 KiB pages. It revalidates `project.json`, copies
only declared inputs into a fresh `build:native` tree as TEXT/ttxt, and uses the
same `BuildPlan` command generator as the fallback shell recipe. No extra job
input or model-supplied shell command is introduced.

MrC/PPCLink/Rez run one at a time through queued Apple events. Replies retain raw
statuses in stdout; MPW status 2 maps to failure 1 and -1 to 127. Other statuses
outside 0–255, malformed/oversized/binary text replies, disappearance, send errors,
Stop and deadlines produce uncertainty, with no subsequent command or replay.
Logs are appended/read back in bounded pages. Success requires the PowerPC PEF
header, both forks, Finder APPL metadata, a verified success marker, and the
existing persisted artifact authorization. `read_build_log` and `run_application`
retain their envelopes and authority checks.

The executor outlives the observing chat run. An abandoned in-flight request
holds the worker lock until its late reply drains or ToolServer disappears.
`native-unknown` and the claim stay as evidence without a terminal result;
`native-drained` records an abandoned late reply. Quitting
Sherclawk with an outstanding command leaves the lock for manual inspection.
The app never scans or replays old jobs. Queue preferences, queue creation,
status UI and a Serve Build Queue toggle remain later increments.

Build/publish the integrated error–repair–rebuild and authorized-launch fixture:

```bash
APP=SherclawkSelfBuildCheck tools/deploy-to-share.sh
APP=SherclawkSelfBuildStopCheck tools/deploy-to-share.sh
```

Run with MacRelix quit and the queue unowned. It checks independent and starter
projects with multiple sources, explicit native ownership, resource structure,
compiler failures, revision-bound edits and fresh build IDs. Evidence is recorded
in `Retro68:SherclawkSelfBuildCheck.log`; acceptance is recorded below.

### Fixed native build diagnostic (idea 005, increment 2)

With MacRelix quit and ToolServer idle, publish and launch:

```bash
APP=SherclawkNativeBuildCheck tools/deploy-to-share.sh
APP=SherclawkNativeBuildErrorCheck tools/deploy-to-share.sh
```

Launch each separately in Finder and save its root log before repeating it:
`Retro68:SherclawkNativeBuildCheck.log` or
`Retro68:SherclawkNativeBuildErrorCheck.log`. The good diagnostic stages exact
current starter sources into a fresh `Retro68:NativeBuildCheck<ticks>:` folder,
then sends separate queued MrC, PPCLink and Rez commands through `toolserver.c`.
It verifies both forks, the PowerPC PEF header, `cfrg`, `SIZE`, Finder `APPL/SHTP`
and the closed success record before native launch. Expected title/text in the
launched window supply visual acceptance beyond the recorded process observation.
The error variant expects a deliberate MrC failure and verifies there is no
application, success record or launch. Each attempt retains its own inputs.

`S` or Command-period stops observation and subsequent steps; an outstanding
reply is drained without advancing. A 120-second command deadline or observed
ToolServer disappearance likewise records an unknown outcome. Command-Q leaves
the diagnostic. Never launch a partial artifact from a stopped attempt.
Process scans refuse this fixture while the known MacRelix app is running;
this is not queue locking or universal executor detection.

Both success runs, compiler failure and Stop/late-reply behavior passed in OS 9.
The repeated artifact payloads matched except for a PEF timestamp; full resource
maps also differed, so complete fork determinism is not claimed. Detailed
fixtures and timings are in [idea 005](docs/idea-005-sherclawk-queue-mode.md).
This fixed diagnostic remains separate from the integrated self-build executor above.

### Running applications

`run_application(build_id)` takes only the exact successful build ID, never a
path or shell command. It resolves the persisted authority and artifact through
non-alias ancestors, checks Finder `APPL`, and rehashes both forks in <=1 KiB
steps while servicing events. Each fork must be nonempty and <=1 MiB. Stop or
a one-minute verification deadline prevents launch. Changed, missing, partial,
failed or unknown artifacts are refused; editing project sources does not alter
the older recorded build or cause an implicit rebuild.

Before calling `LaunchApplication`, the app journals `run_intent` and reserves
a unique `run-...` file of Finder type `ShRR` in the queue, with closed read-back
and volume flush. It uses `launchContinue`/`launchDontSwitch`: Sherclawk keeps
running and the application need not become foreground. The returned native
process identity is checked against the exact artifact FSSpec and journaled as
`run_observed`, with `run_id`, `build_id`, snapshot and artifact path. An already
running application may be reused by the Process Manager. The observation means
`process_present`; `smoke_test: not_performed` explicitly leaves functional and
visual acceptance to subsequent checks. There is no exit monitoring, screenshot
capture, app termination or automatic recovery in this tool.

Launch errors, failed process observation and failed post-launch journaling are
`uncertain` and stop the agent. Never retry automatically: inspect retained run
intent and session records first. Fork fingerprints are observational, not
cryptographic; AFP-server writes and a change between final verification and
launch remain outside File Manager guarantees. Native records are local to this
version/architecture; AFP FlushVol is not a power-loss durability guarantee.

Creation journals intent, reserves a unique sibling staging folder, writes
and closes each file, flushes and verifies exact bytes and metadata, journals
the staged project, then publishes with one non-overwriting folder rename.
It verifies the published folder identity and files before journaling completion
and returning `CREATED_PROJECT`. A failed stage remains at `temporary_path`;
a publication or completion-record failure returns `uncertain`. Failures after
staging begins stop the run. There is no automatic cleanup, rollback or retry.
Inspect the journal and both paths before recovery. FlushVol does not establish
power-loss durability over AFP. This fixed small template executes synchronously;
Stop prevents the next tool call and does not undo a completed project.

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

## Controls, limits and sessions

The sidebar places the subtitle below Sherclawk's title and shows current
model-history usage below the artwork as used/capacity KiB and a percentage.
Usage updates during runs and resets with New Chat or a successful handoff.

New `ppc-toolbox-v1` projects include confirmed close-box handling as well
as Command-Q. Preserve these event branches when adding application behavior;
existing projects are not changed by updating the embedded template.

Send or Command-Return starts a run. The model is fixed for that run. The app
services events while waiting, executes calls sequentially, displays tool
results, and requests another model response until it gets a final answer.
Stop or Command-Period prevents new execution and records interrupted results
for pending calls; completed results remain in history. New Chat starts a fresh session.

**Save Handoff**, right-aligned beside the bottom controls, is also available
as **File > Save Handoff (Command-H)**. The button and menu item are disabled
during runs and when there is no conversation to summarize. It summarizes a
stopped or completed conversation with a separate, tool-free model request. This still works when
history is full: the summary request never appends to the old history. The model
produces a concise Markdown handoff covering goals and constraints, completed
work and exact paths, observed verification, unresolved or uncertain operations,
and next steps. Summary generation uses the selected model and the normal
120-second HTTPS deadline. Stop cancels observation and retains the old history;
there is no automatic retry or automatic compaction.

The app saves a unique `hXXXXXXXX.md` in `Retro68:Sherclawk Sessions:` as
MacRoman/CR plain `TEXT` (at most 4096 bytes), so later sessions can inspect it
with `list_files` and `read_text`. It closes, flushes, and reads back the exact
bytes before creating a new UTF-8 journal with the summary as its first user
message. Only then does it replace model history. Truncated/tool-call responses,
unsupported encoding, oversized summaries, and persistence failures leave the
old history and journal intact. A partially created Markdown file is retained
and reported; incomplete candidate journals remain in the sessions folder.
Both the Markdown and seeded message name the original journal. The visible
transcript and any unsent prompt remain available; send a message to continue.
At 75% history usage, the status suggests saving a handoff. The summary is lossy:
current sources and uncertain mutations still need inspection before acting.
Full-journal reloading remains unimplemented; to resume after quitting, ask a
new chat to read the saved Markdown path displayed when it was created.

The Save Handoff button was built, published fork-aware and checked in OS 9.2.2
on October 6, 2026: disabled in a fresh chat and during a model request, enabled
after a reply, and a click saved the handoff and prepared fresh history.

Limits are explicit: 32 model rounds, 64 executed calls per run, four calls per
response, 8 KiB arguments per call, 256 KiB history, 288 KiB JSON request, 64 KiB
raw HTTP response, and 3,072 output tokens. Each HTTPS request has a 120-second
deadline. Tool output is below 1,536 bytes; folder listings have cursors and
text reads provide `next_byte` continuation when a line is partial. Reads scan
at most 8 KiB per invocation. Whole-file revisions guard small-file edits;
larger-file scan revisions are observational.
Token-truncated tool calls never execute. There is no automatic network retry.
Reaching a run limit pauses with the model-round and tool counts, history usage
percentage, and a reminder to send Continue. Sending another message resets
the run counters while retaining conversation history; handoff is not required.

History buffers are static: the app allocates their full capacity at launch,
not incrementally as messages arrive. With handoff enabled, each additional
history byte costs roughly four RAM bytes (current history, candidate history,
JSON request, and HTTP request). Per-response scratch is bounded separately.
The 256 KiB build has 2,330,176 bytes (2.22 MiB) of linked code/static data;
this excludes dynamic TLS/UI allocations and the stack. Its `SIZE` resource
still requests 8 MiB preferred / 4 MiB minimum. A 512 KiB history would raise
that baseline to roughly 3.22 MiB, making the 4 MiB minimum tight; 1 MiB history
would need roughly 5.22 MiB before dynamic allocations, and 2 MiB would exceed
the current 8 MiB preferred allocation. Re-measure and raise `SIZE` before such
increases. Larger histories also upload more bytes and consume more model input
tokens on every round; byte capacity is not a guarantee of provider context
capacity. The 30,001-byte TextEdit transcript remains independently bounded.

Before execution, complete assistant responses and tool-start records are
saved to unique UTF-8 JSON-lines files in `Retro68:Sherclawk Sessions:`.
Results and user messages are saved there too. These files contain conversation
and file contents; lifecycle logs omit them and credentials. Rejected model
responses never enter the conversation: a `model_error` journal record keeps
their HTTP status, received body size and reason, and every stopped run appends
its reason to the lifecycle log so failures stay diagnosable after relaunch.
A recording failure stops execution. Journals are preserved across relaunches;
automatic session reloading/recovery is not implemented yet. When display
limits are reached, earlier visible text is replaced with a notice referring to
the saved session; model history is not silently dropped.

## Files and verification

| Path | Purpose |
|---|---|
| `main.c`, `hello.r` | Toolbox UI, character artwork, live history indicator, session journal, cooperative scheduling |
| `agent.c`, `agent.h` | Typed provider history, tool-call/result pairing, bounds and Stop |
| `tools.c`, `tools.h` | Native environment/catalog/text executors, journaled creates and guarded exact edits |
| `inspect.c`, `inspect.h` | Read-only resource, Finder identity, alias, process and text-metric executors |
| `json.c`, `text.c`, `network.c`, `chat.c` | Copied protocol/transport/display foundation and baseline checks |
| `tools/embed-project-template.py` | Embed the verified C/Rez source and protocol-2 descriptor under ignored `build/` |
| `tools/project-check.c` | Native create_project publication, exact bytes, metadata and collision diagnostic |
| `tools/make-art.py` | Stdlib PNG decoder and native icon/window resource conversion |
| `tools/netatalk_meta.py`, `tools/deploy-to-share.sh` | Fork-aware publication with the new Finder identity |
| `tools/check.sh`, `tools/check-transport.sh` | ASan/UBSan protocol, loop, create/edit faults, revision/encoding and TLS I/O checks |
| `tools/handoff-check.c` | Guest File Manager diagnostic: Markdown verification, new journal seed, distinct filenames and retention on encoding/size failures |
| `tools/probe.c`, `tools/build-host-probe.sh` | Real model/tool/follow-up diagnostic on host and guest |
| `tools/write-check.c` | Native create/read/collision, MacRoman/CR/TEXT and size-boundary diagnostic |
| `tools/search-check.c` | Native recursive search, continuation, MacRoman and discovery/read/edit/read diagnostic |
| `tools/edit-check.c` | Native guarded replacement, backup, pagination, busy-file, encoding and boundary diagnostic |
| `tools/inspect-check.c` | Native read-only inspection acceptance with retained Finder-format `alis`/0 alias and resource fixtures |
| `tests/toolbox/`, `tests/test_tools.c` | File Manager model for mutation journal barriers, I/O faults and rename races; read-only model for resource maps, aliases, processes, fonts and text metrics |
| `tools/scroll-check.c` | Copied actual Toolbox scrollbar diagnostic |
| `tools/guest-input.py` | Non-overlapping QMP typing and 250ms control clicks through the UTM helper |
| `PLAN.md` | Artwork requirements and remaining coding-harness milestones |
| `templates/ppc-toolbox/` | Native MPW PowerPC template and guest verification record |
| `tools/materialize-native-template.py` | Create new MacRoman/CR template source and LF shell scripts |
| `toolserver.c`, `toolserver.h` | Single-command queued ToolServer client, correlated replies, bounded diagnostics and abandoned-outcome draining |
| `tools/native-build-check.c` | Fixed native compile/link/Rez/verify/launch and deliberately failing compiler acceptance fixtures |
| `tools/toolserver-check.c`, `tools/toolserver-check.r` | Standalone queued ToolServer replies, native discovery/launch, retained Rez fixtures and explicit unknown-outcome fault controls |
| `tools/native-process-check.c` | Native Process Manager diagnostic for executor paths |
| `selfbuild.c`, `selfbuild.h`, `build_plan.h` | Exclusive current-snapshot executor, bounded copies/logs, compatible results, unknown-outcome draining |
| `toolserver.c`, `toolserver.h` | Single-command queued Apple-event client with sender/return-ID correlation |
| `tests/test_selfbuild.c` | Ownership races, snapshot changes, command failures, Stop/deadline, late replies and authorization fault model |
| `build_project.c`, `build_project.h` | Descriptor validation, trusted recipe, revision snapshot, cooperative build and log pages |
| `tools/build-check.c`, `tests/test_build_project.c` | Multi-source/error–repair–rebuild/self-build-and-launch diagnostics and host fault checks |
| `run_application.c`, `run_application.h` | Persisted artifact authority, bounded fork verification, native launch and run observations |
| `jobs.c`, `jobs.h` | Native trusted snapshot publication and bounded completion/log polling |
| `tools/job-check.c`, `tests/test_jobs.c` | Event-driven guest diagnostic and publication/polling fault checks |
| `worker/` | MacRelix file-job protocol, executor, native build wrapper and guest evidence |
| `tools/publish-worker-job.py`, `tests/test_worker.py` | Diagnostic job producer and publication/execution/crash checks |

```bash
tools/check.sh
tools/check-transport.sh
APP=SherclawkHandoffCheck tools/deploy-to-share.sh
# Launch in OS 9; inspect Retro68:SherclawkHandoffCheck.log.
APP=SherclawkProjectCheck tools/deploy-to-share.sh
# Launch in OS 9; inspect Retro68:SherclawkProjectCheck.log.
python3 tests/test_worker.py
APP=SherclawkRunCheck tools/deploy-to-share.sh
# Launch in OS 9; inspect Retro68:SherclawkRunCheck.log.
APP=SherclawkJobCheck tools/deploy-to-share.sh
# See worker/README.md for the guest native producer/poller diagnostic.
tools/build-host-probe.sh
build/host-probe
./build.sh SherclawkProbe_APPL
APP=SherclawkProbe tools/deploy-to-share.sh
# Launch SherclawkProbe in OS 9; it logs, then quits.
ssh "$SHARE_HOST" 'cat /srv/retro68/SherclawkProbe.log'
APP=SherclawkWriteCheck tools/deploy-to-share.sh
# Launch SherclawkWriteCheck in OS 9; fixed diagnostic logs, then quits.
ssh "$SHARE_HOST" 'cat /srv/retro68/SherclawkWriteCheck.log'
APP=SherclawkSearchCheck tools/deploy-to-share.sh
# Launch SherclawkSearchCheck in OS 9; retains a unique source/backup fixture.
ssh "$SHARE_HOST" 'cat /srv/retro68/SherclawkSearchCheck.log'
APP=SherclawkEditCheck tools/deploy-to-share.sh
# Launch SherclawkEditCheck in OS 9; preserves its unique fixture and backups.
ssh "$SHARE_HOST" 'cat /srv/retro68/SherclawkEditCheck.log'
APP=SherclawkInspectCheck tools/deploy-to-share.sh
# Launch SherclawkInspectCheck in OS 9; retains alias/resource fixtures.
ssh "$SHARE_HOST" 'cat /srv/retro68/SherclawkInspectCheck.log'
```

The cursor matcher can mistake highlights in the lobster artwork for the arrow.
After using UTM, check `info mice`; this VM needs `mouse_set 4` for the
working relative HID mouse. The active absolute tablet ignores relative input.
For UI diagnostics, reanchor, use blind `moveto`, verify a screenshot, then
use `tools/guest-input.py --click` with the pointer still.

The host probe labels its simulated file executor explicitly; it checks actual
provider protocol and transport. The guest probe uses actual native tools and a
fixed `Sherclawk Fixture.txt`, plus path, binary-file, duplicate-argument and
unknown-tool checks. Only fixed diagnostic conversations are printed. The write diagnostic preserves
a unique fixture folder; its log includes flushed mutation recovery records.

Recorded October 5, 2026: 256 KiB history / 288 KiB request PowerPC build and
ASan/UBSan checks passed, including a near-full history handoff request and
refusal of truncated/tool-call summaries and failed seed persistence. The
OS 9 handoff diagnostic reported zero failures: exact Markdown read-back,
new-journal seeding, distinct filenames, and retention on encoding/size failures.
A live `openai/gpt-6-luna` conversation was summarized using Command-H, saved
as native Markdown, and continued from the seeded history.

Recorded October 2, 2026: PowerPC build passed; ASan/UBSan baseline, agent-loop
and TLS application-I/O checks passed. Live host and OS 9.2.2 probes both called
all three tools, read `copper-crab` from the fixture, and received a final
model response over TLS 1.3; each reported `RESULT failures=0`. Finder displayed
the lobster icon, including the application menu, and the main window displayed
the matching character. The main app also completed the fixture-read request
and persisted its user, assistant, call-start and tool-result records. Stop
was verified in host protocol checks and through Command-Period in the guest,
where completed tool results remained visible. A subsequent fixture request
completed and its journal parsed successfully. The AFP share's earlier
spike/recon scripts, outputs, and test folders were archived by same-volume
rename into `Retro68:Spikes:`. Superseded apps and diagnostics from before the
rename are in `Retro68:Older:`; their earlier session directory remains preserved.

Create-only write verification, October 2, 2026: the PowerPC app built and was
published fork-aware to the AFP share. ASan/UBSan protocol and File Manager
model checks passed, including journal failures before staging/after publish,
short writes, corrupt reads, rename races, uncertain outcomes, malformed calls,
encoding/control-byte refusal, size limits and long Unicode recovery paths.
The OS 9.2.2 `SherclawkWriteCheck` diagnostic reported `RESULT failures=0` for
native create/read/collision behavior, exact MacRoman/CR bytes, Finder `TEXT`,
matching revisions, empty files and the 4 KiB boundary. In the main app,
`openai/gpt-6-luna` created `ClawWrite.c`, read it, received `EXISTS` for a second
create with different contents, and reread the unchanged original before a
final response. All six tool calls/results paired in the parsed session, with
intent/staged/committed recovery records and matching revisions. Command-Period
stopped a follow-up request before `ClawStopped.c` was created, retaining the
completed result and original file. Injected I/O/recovery failures were tested
on the host model, not by damaging the live guest volume.
After the final DEL-byte and Unicode-path hardening, the rebuilt/published main
app created `ClawFinal.c`, read back `lobster`, and received a final model answer;
its saved session parsed successfully.

Exact-edit verification, October 2, 2026: ASan/UBSan checks passed for
whole-file/page revisions, source changes at unchanged size/date, changed
catalog identity, unique and overlapping matches, deletion, encoding/control
refusal, size and recovery-path bounds, busy opens, journal barriers, corrupt
and short staging I/O, publication races, swapped output identity and uncertain
outcomes. Agent checks reject truncated edit calls and pair a stopped pending
edit with an interrupted result. TLS application-I/O checks also passed.
The PowerPC `SherclawkEditCheck` was published fork-aware and run on the live
OS 9.2.2 AFP volume; the final diagnostic reported `RESULT failures=0 stopped=0`
for exact replacement, retained original bytes, Finder `TEXT`, MacRoman/CR,
page-independent revisions, stale/missing/overlapping-match refusal, busy-file
refusal, deletion to empty and the 4 KiB boundary. The rebuilt main app's
`openai/gpt-6-luna` run created and read `ClawEdit.c`, changed `return 0;` to
`return 1;` through `edit_text`, reread it and reported the backup path in its
final response. All six calls/results paired in the parsed session; the edit
had intent/staged/backed-up/committed records and matching readback revision.
An additional model edit restored `return 0;` and preserved a second backup.
A held Command-Period stopped a subsequent request to change it to `return 2;`;
the source stayed unchanged, both backups remained intact, and the parsed
session retained the completed calls without a subsequent mutation.
Injected failure paths were tested on the host model, not by damaging the
live guest volume.

Search verification, October 2, 2026: text tools v2 built and was published
fork-aware. ASan/UBSan checks passed for recursive/nonrecursive discovery,
CR/CRLF line numbers, pagination, overlapping and scan-boundary matches,
MacRoman queries, resource/binary/alias refusal, malformed/overflowed cursors,
empty initial cursors, duplicate arguments and read-close errors. Agent checks
advertise search and pair a stopped pending search with an interrupted result.
The OS 9.2.2 native `SherclawkSearchCheck` diagnostic reported
`RESULT failures=0` for recursive discovery, continuation, MacRoman search
and a search/read/guarded-edit/read cycle with a preserved backup.
The final main app displayed `Text tools: v2`; its `openai/gpt-6-luna`
conversation called `search_text` with an empty initial cursor, found
`silver lobster` in `Sherclawk Search 0013438b:Sources:hello.c` on line 2,
read the source and returned the correct final answer. Both tool calls/results
paired in the parsed `s00135466.jsonl` session. Earlier testing exposed the
empty-cursor compatibility issue; the final build accepts it as a new search.

Share cleanup retained the active app, lifecycle log and session journals.
Completed v1 and v2 diagnostics, sidecars and source/backup fixtures were moved
by same-volume rename into `Retro68:Spikes:Text tools v1 2026-10-02:` and
`Retro68:Spikes:Text tools v2 2026-10-02:`. Archive manifests record the old
root locations; saved recovery/session paths refer to those original locations.
No AFP service restart was needed.


Native job integration, October 3, 2026: `jobs.c` now reserves fresh job folders,
stages and reads back closed snapshot inputs in bounded steps, publishes ready
last, and polls strict terminal records plus separate 1 KiB log pages. The
PowerPC `SherclawkJobCheck` diagnostic was built, published fork-aware and
launched in OS 9.2.2. It reported `RESULT failures=0` for a 3 KiB native snapshot
and stdout/stderr continuation through the MacRelix worker. Command-period
recorded an unknown outcome without changing the published job; a later explicit
worker scan completed that original queued job. ASan/UBSan File Manager fault
checks and the existing Python worker checks passed. Detailed evidence and
limits are in [worker/VERIFIED.md](worker/VERIFIED.md). Model-facing build/run
tools, source revision binding and artifact authorization remain planned.

Create-project verification, October 5, 2026: ASan/UBSan protocol and File Manager
checks passed for exact template bytes, MacRoman/CR/TEXT metadata, nested
parents, malformed arguments, path/alias refusal, existing destinations, short
writes, corrupt reads, close failures, journal barriers, rename collisions,
changed folder identity and uncertain publication. The final PowerPC
`SherclawkProjectCheck` was built, published fork-aware and launched on the live
OS 9.2.2 AFP volume. It reported `RESULT failures=0` and retained
`Retro68:Sherclawk Project 0000af4e:`. All three files matched the embedded
template, had Finder `TEXT`/`ttxt` with no resource fork, and `main.c` returned
an editable whole-file revision. Recreating the project and escaping the
workspace were refused without another mutation. Injected failure paths were
tested on the host model. This acceptance covers project creation, not native
compilation or launch of the resulting project.

The rebuilt main Sherclawk app was also published and launched. Its live
`openai/gpt-6-luna` conversation created `ClawProject01` with `create_project`,
read all 1,671 source bytes through six bounded `read_text` calls and returned
a final answer. All seven calls/results paired in `s0000c375.jsonl`; concatenated
readback matched the repository template exactly and all pages carried the same
editable whole-file revision. Diagnostic evidence is retained locally under
ignored `build/project-check-verified.log` and `build/project-session.jsonl`.

Native build verification, October 5, 2026: PowerPC main/diagnostic builds and
ASan/UBSan protocol, descriptor and File Manager fault checks passed. The guest
`SherclawkBuildCheck` reported `RESULT failures=0` for independent and starter
projects with two C sources and editable output names. Each route deliberately
failed compilation, repaired the source with a revision-guarded edit and submitted
a fresh successful build. The independent project also used a declared header
and include path. Successful artifacts were manually opened from their exact
snapshot paths in OS 9: the independent window displayed text from its second C
file, and the starter displayed the native template window. This is visual
launch evidence; automated run IDs/runtime observations remain planned. Retained
build IDs and compiler results are recorded in [worker/VERIFIED.md](worker/VERIFIED.md).
The initial independent fixture exposed a missing `QDGlobals` definition; the
linker failure was reported correctly and retained. Host checks include malformed
descriptors, duplicate/unsupported settings, path metacharacters, source changes,
close faults, publication/Stop, artifact resource-fork refusal and worst-case
log escaping. Raw evidence is under ignored `build/build-check-verified.log`,
`build/build-independent.png`, and `build/build-artifacts2.png`.

The final main app's live `openai/gpt-6-luna` loop also built the independent
fixture, received build ID `build-0002ed03-0001`, read both stdout pages (0–128,
128–151), and returned a final successful build answer. All six calls/results
paired in `s0002e7bf.jsonl`; the local ignored copy is `build/build-session.jsonl`.
The final folder-path compatibility fix accepts trailing colons from catalog
results. Stop retained an uncertain published job without resubmission; its
original snapshot later completed when the worker was explicitly resumed.


Native launch verification, October 5–6, 2026: the final PowerPC main and
`SherclawkRunCheck` builds passed, along with ASan/UBSan fault checks. The OS 9
build-and-launch diagnostic reported `RESULT failures=0` for independent and
starter projects: compiler failures refused launch, revision-guarded repairs
produced fresh successful snapshots, and both exact artifacts returned distinct
run IDs and native process observations. The main app's live
`openai/gpt-6-luna` loop also launched a retained authorized build from a fresh
session and received a final response, verifying persisted authority across app
lifetimes. These are process observations, not functional smoke-test results.
IDs and retained evidence are in [worker/VERIFIED.md](worker/VERIFIED.md).


ToolServer spike verification, October 6, 2026: the standalone PowerPC diagnostic
built and was published fork-aware, then ran in OS 9.2.2. Native desktop-database
ToolServer launch, queued success/error Rez replies, exact compiled string-resource
readback, and synthetic malformed-reply checks passed (`RESULT failures=0`).
Stop and expired-deadline runs retained unknown outcomes and drained late replies
without resend. A polite mid-command ToolServer quit was deferred; an explicit
fresh suite relaunched it and passed. Sends took zero or one guest tick without
switching focus. A background run received its reply, but had a maximum event-loop
gap of 303 ticks (about five seconds). Abrupt process death and reply delivery
during window dragging remain untested. This is ToolServer-channel evidence,
not acceptance of a native queue executor or full C build. Detailed IDs, retained
fixtures and limitations are in [idea 005](docs/idea-005-sherclawk-queue-mode.md);
raw evidence is under ignored `build/toolserver-check-verified.log` and
`build/toolserver-final.png` / `build/toolserver-background.png`.


Fixed native executor verification, October 6, 2026: both PowerPC diagnostics
built and were published fork-aware. With MacRelix quit, two native template
builds passed compile/link/Rez, fork/resource/metadata verification and exact
artifact launch (`RESULT failures=0`); their windows displayed the expected text.
A deliberate MrC error passed refusal checks with no artifact, success record
or launch. Stop during linking retained an unknown outcome and drained a late
reply without Rez or launch. The reusable ToolServer client and fixed diagnostic
are separate from the queue and model tools. Retained fixture IDs, repeat-byte
comparison limits and ignored raw evidence are in
[idea 005](docs/idea-005-sherclawk-queue-mode.md#increment-2-guest-evidence--october-6-2026).

Integrated self-build verification, October 6, 2026: `SherclawkSelfBuildCheck`
reported `RESULT failures=0` in OS 9.2.2 with MacRelix absent before each build.
Independent and starter projects each passed compiler-error, repair, authorized
launch and fresh revision-bound rebuild fixtures: six retained build IDs, four
successful launches, two correctly refused failed builds. Resource checks read
`cfrg`/0 and `SIZE`/-1 for every successful artifact. The independent window showed
both the original text and the edited “Fresh revision compiled natively” text.
The detailed build IDs and retained evidence are in
[idea 005](docs/idea-005-sherclawk-queue-mode.md#increment-3-guest-evidence--october-6-2026).
Host ASan/UBSan checks and all seven Python worker compatibility tests pass.

The integrated Stop fixture also reported `RESULT failures=0`: first MrC command
abandoned, late status-0 reply drained, no subsequent command/result/authority/
success/launch, and lock released after draining. Its retained ID is
`build-00013dab-0001`; `native-unknown`, `native-drained` and the claim remain.
This demonstrates observation stopping and safe draining, not cancellation.


Read-only inspection verification, October 6, 2026: the PowerPC main app,
`SherclawkProbe` and `SherclawkInspectCheck` builds passed along with the
ASan/UBSan host suite, whose File Manager model now also covers resource
maps, alias records, Process Manager paging, Font Manager families and
QuickDraw text metrics. The guest diagnostic reported `RESULT failures=0`
(49 PASS, 0 FAIL) in OS 9.2.2 against the retained
`Retro68:Sherclawk Inspect 0003933d:` fixtures: Finder identity, flags and
deterministic dates; alias resolution to sibling and folder targets; Process
Manager paging including self/front; 24 installed families including Chicago;
pixel metrics and style bits; resource-map paging; `STR `, `TEXT`, `vers`
and hex reads including a 300-byte two-page continuation; plus missing-file,
non-alias, no-fork, malformed-type and out-of-range refusals. The live
`openai/gpt-6-luna` guest probe then reported `RESULT failures=0` for a
six-tool conversation (`get_environment`, `list_files`, `read_text`,
`get_file_info`, `list_fonts`, `measure_text`) over TLS; the model supplied
both `font` (empty) and `font_id`, which drove the accept-when-consistent
rule and the empty-name-as-absent handling. Logs are retained locally under
ignored `build/inspect-check-verified.log` and `build/probe-verified.log`.
Resource reads never load a whole fork, and inspection results are evidence,
never launch authority.
