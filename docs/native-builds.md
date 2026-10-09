# Native project builds and application launch

Sherclawk can create a starter project or build a valid descriptor assembled
with ordinary file tools. Builds use the guest's MrC/PPCLink/Rez toolchain;
compiling Sherclawk itself on the host is a separate
[development workflow](development.md#build-and-publish).

The [architecture](architecture.md#buildrun-contract) describes snapshot and
launch authority. [Developer diagnostics](development.md#native-build-diagnostics)
exercise the native executor and its failure paths; dated acceptance is in the
[verification history](history/verification.md).

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

The starter demonstrates a real Control Manager Clear button and a TextEdit
field. Sherclawk's system policy requires suitable standard Toolbox components
for application UIs and recommends bounded application-side logging for runtime
diagnosis. The starter writes a fresh, at-most-4096-byte MacRoman/CR `TEXT`
`runtime.log` beside its built executable, recording lifecycle events, actions
and failure codes without typed text or secrets. Read it with `read_text` using
the artifact folder from the build result; it is separate from build stdout/
stderr. Save it before relaunching, which resets it. See the
[template guide](../templates/ppc-toolbox/README.md#runtime-diagnosis) for limits
and guest interaction checks. Existing projects retain their original sources.

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

## Native project builds

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

Sherclawk creates the queue itself before its first publication: it resolves
the workspace root as a non-alias folder, creates `Worker01` then `buildjobs`
one level at a time (journaled intent and verification, refusing a file or
alias in either name) and only then publishes. Sherclawk acquires the existing
`worker-lock` and executes its current snapshot itself when unowned. ToolServer
and its installed SDK are still required; MacRelix is unnecessary for this
path. General queue service is not implemented. The host step below is only
needed when an external worker should serve the queue before the app has
created it:

```bash
ssh "$SHARE_HOST" 'sudo -n install -d -o macos9 -g macos9 -m 775 /srv/retro68/Worker01/buildjobs'
```

For the existing fallback, start the worker before publishing builds. Publish
`worker/worker.pl` under a fresh filename first (see
[worker/README.md](../worker/README.md)):

```sh
perl -w /Volumes/Retro68/Worker01/<published-worker-name>.pl /Volumes/Retro68/Worker01/buildjobs
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

## Native executor

`build_project` publishes the same job-queue (protocol 1) snapshot, then attempts exclusive
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
The app never scans or replays old jobs. Automatic queue creation is implemented. Queue preferences, status UI and a
Serve Build Queue toggle remain future work.

### ToolServer protocol

The client in `toolserver.c` speaks the same Apple-event protocol as the
upstream `tlsrvr` tool.

- **Discovery.** Find a running process with signature `'MPSX'`; otherwise find
  the ToolServer `APPL` through the mounted volumes' desktop databases and call
  `LaunchApplication`.
- **Request.** One `kAEMiscStandards`/`kAEDoScript` event per MPW command,
  with the script as a `typeChar` direct object in MacRoman/CR text:
  `Set Exit 0`, `Directory "<cwd>"`, `<command> < Dev:Null`,
  `Set CommandStatus {Status}`, `Directory "{MPW}"`, `Exit {CommandStatus}`.
- **Send.** Asynchronously with `kAEQueueReply | kAENeverInteract`. `tlsrvr`
  itself blocks with `kAEWaitReply`, which would freeze the UI for the length of
  every compile. `kAENeverInteract` is the API flag; foreground switching is a
  separate concern and was verified in the guest, not assumed.
- **Reply.** `'stat'` (`typeSInt32`) is the MPW status, the direct object holds
  stdout and `'diag'` holds diagnostics, each as bounded `typeChar` text.
  Replies are matched on return ID and sender PSN; anything unverifiable,
  malformed or oversized is uncertain.
- **Status mapping.** `-1` becomes 127 and `2` becomes 1; other values in
  0–255 pass through; everything else is uncertain. Upstream also maps a
  user-cancel reply (Command-period) to 128 and quits and retries once on an
  out-of-memory error. Sherclawk does neither: cancellation is not claimed, and
  an uncertain outcome is never replayed.

## Running applications

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

## Owned process lifetime

A successful launch now reports whether graceful Quit is supported. Only a new
process absent from the complete prelaunch snapshot, matched to the verified
artifact and launched by Sherclawk, can be owned. Authority is granted after
`run_observed` is journaled and lives in memory across New Chat. An already
running app gets no new handle; `original_run_id` identifies an existing owned
handle when available. Tracking errors or capacity exhaustion do not block
launches but return `quit_supported:false`. Journals and launch authorization
records do not restore process ownership after restarting Sherclawk.

`quit_application(run_id)` requests noninteractive normal Quit and
observes process exit asynchronously for 30 seconds. It never force quits,
discards changes, retries a send, or cleans up apps automatically. Stop after
submission cannot cancel Quit. See the [implementation spec](quit-application.md)
and [tool contract](tools.md#quit-an-owned-application).
