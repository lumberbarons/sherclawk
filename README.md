# Sherclawk — The consulting crustacean

An independent source copy of HelloChat. The original `../hello-chat/` remains
unchanged. Sherclawk adds a native, sequential agent loop over direct OpenRouter
HTTPS, a Finder icon based on `../sherclawk.png`, and that character in the main
window. [PLAN.md](PLAN.md) tracks the full coding-harness roadmap.

The installed tools are `get_environment`, `list_files`, `read_text`,
create-only `write_text`, and revision-guarded `edit_text`. Native build jobs
and artifact launch come next. The model receives only these installed capabilities; the app executes tools
with the File Manager and records their results before requesting a follow-up.

## Build and publish

Uses HelloHTTPS's existing Certainly staging, patches, Universal Interfaces,
and Retro68 Docker image. No additional imaging or runtime libraries are
required. The stdlib-only artwork converter expects the workspace reference
PNG (8-bit RGBA, noninterlaced); override its path with `SHERCLAWK_ART`.

```bash
cp sherclawk/config.example.h sherclawk/config.local.h
# Set SHERCLAWK_API_KEY and optionally SHERCLAWK_MODEL / SHERCLAWK_WORKSPACE.
sherclawk/build.sh
sherclawk/tools/deploy-to-share.sh
```

A local config was copied from HelloChat for this workspace and renamed to
Sherclawk's macros. It is ignored, and credentials are embedded only in local
binaries. Builds without credentials still launch. Quit a running copy before
publishing/relaunching. Normal runtime uses no host executor or model relay.

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

Send or Command-Return starts a run. The model is fixed for that run. The app
services events while waiting, executes calls sequentially, displays tool
results, and requests another model response until it gets a final answer.
Stop or Command-Period prevents new execution and records interrupted results
for pending calls; completed results remain in history. New Chat starts a fresh session.

Limits are explicit: 16 model rounds, 32 executed calls per run, four calls per
response, 8 KiB arguments per call, 64 KiB history, 96 KiB JSON request, 64 KiB
raw HTTP response, and 3,072 output tokens. Each HTTPS request has a 120-second
deadline. Tool output is below 1,536 bytes; folder listings have cursors and
text reads provide `next_byte` continuation when a line is partial. Reads scan
at most 8 KiB per invocation. Whole-file revisions guard small-file edits;
larger-file scan revisions are observational.
Token-truncated tool calls never execute. There is no automatic network retry.

Before execution, complete assistant responses and tool-start records are
saved to unique UTF-8 JSON-lines files in `Retro68:Sherclawk Sessions:`.
Results and user messages are saved there too. These files contain conversation
and file contents; lifecycle logs omit them and credentials. A recording failure
stops execution. Journals are preserved across relaunches; automatic session
reloading/recovery is not implemented yet. When display limits are reached,
earlier visible text is replaced with a notice referring to the saved session;
model history is not silently dropped.

## Files and verification

| Path | Purpose |
|---|---|
| `main.c`, `hello.r` | Toolbox UI, character artwork, session journal, cooperative scheduling |
| `agent.c`, `agent.h` | Typed provider history, tool-call/result pairing, bounds and Stop |
| `tools.c`, `tools.h` | Native environment/catalog/text executors, journaled creates and guarded exact edits |
| `json.c`, `text.c`, `network.c`, `chat.c` | Copied protocol/transport/display foundation and baseline checks |
| `tools/make-art.py` | Stdlib PNG decoder and native icon/window resource conversion |
| `tools/netatalk_meta.py`, `tools/deploy-to-share.sh` | Fork-aware publication with the new Finder identity |
| `tools/check.sh`, `tools/check-transport.sh` | ASan/UBSan protocol, loop, create/edit faults, revision/encoding and TLS I/O checks |
| `tools/probe.c`, `tools/build-host-probe.sh` | Real model/tool/follow-up diagnostic on host and guest |
| `tools/write-check.c` | Native create/read/collision, MacRoman/CR/TEXT and size-boundary diagnostic |
| `tools/edit-check.c` | Native guarded replacement, backup, pagination, busy-file, encoding and boundary diagnostic |
| `tests/toolbox/`, `tests/test_tools.c` | File Manager model for mutation journal barriers, I/O faults and rename races |
| `tools/scroll-check.c` | Copied actual Toolbox scrollbar diagnostic |
| `tools/guest-input.py` | Non-overlapping QMP typing and 250ms control clicks through the UTM helper |
| `PLAN.md` | Artwork requirements and remaining coding-harness milestones |

```bash
sherclawk/tools/check.sh
sherclawk/tools/check-transport.sh
sherclawk/tools/build-host-probe.sh
sherclawk/build/host-probe
sherclawk/build.sh SherclawkProbe_APPL
APP=SherclawkProbe sherclawk/tools/deploy-to-share.sh
# Launch SherclawkProbe in OS 9; it logs, then quits.
ssh beardmore 'cat /srv/retro68/SherclawkProbe.log'
APP=SherclawkWriteCheck sherclawk/tools/deploy-to-share.sh
# Launch SherclawkWriteCheck in OS 9; fixed diagnostic logs, then quits.
ssh beardmore 'cat /srv/retro68/SherclawkWriteCheck.log'
APP=SherclawkEditCheck sherclawk/tools/deploy-to-share.sh
# Launch SherclawkEditCheck in OS 9; preserves its unique fixture and backups.
ssh beardmore 'cat /srv/retro68/SherclawkEditCheck.log'
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
rename into `Retro68:Spikes:`. Superseded Sherclawd apps and diagnostics are in
`Retro68:Older:`; their earlier session directory remains preserved.

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
