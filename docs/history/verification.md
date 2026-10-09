# Historical verification record

Dated acceptance evidence for the OS 9 guest, host checks and live model runs,
retained from earlier development. Entries describe the state on their recorded
dates. Use the [current guides](../../README.md#documentation) for present behavior.

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
limits are in [worker/VERIFIED.md](../../worker/VERIFIED.md). Model-facing build/run
tools, source revision binding and artifact authorization were planned at this
point and have since been implemented.

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
launch evidence; automated run IDs/runtime observations were planned then and are covered by
the native launch entry below. Retained
build IDs and compiler results are recorded in [worker/VERIFIED.md](../../worker/VERIFIED.md).
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
IDs and retained evidence are in [worker/VERIFIED.md](../../worker/VERIFIED.md).


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
fixtures and limitations are in [native executor evidence](native-executor-evidence.md#increment-1-guest-evidence--october-6-2026);
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
[native executor evidence](native-executor-evidence.md#increment-2-guest-evidence--october-6-2026).

Integrated self-build verification, October 6, 2026: `SherclawkSelfBuildCheck`
reported `RESULT failures=0` in OS 9.2.2 with MacRelix absent before each build.
Independent and starter projects each passed compiler-error, repair, authorized
launch and fresh revision-bound rebuild fixtures: six retained build IDs, four
successful launches, two correctly refused failed builds. Resource checks read
`cfrg`/0 and `SIZE`/-1 for every successful artifact. The independent window showed
both the original text and the edited “Fresh revision compiled natively” text.
The detailed build IDs and retained evidence are in
[native executor evidence](native-executor-evidence.md#increment-3-guest-evidence--october-6-2026).
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

Preferences verification, October 7, 2026: the pure parser/formatter suite runs
under ASan/UBSan in `tools/check.sh` alongside the existing suites: missing
file/defaults, every key, comments and blank lines, CR/LF/CRLF, duplicate and
unknown keys, empty-key clearing, over-long and boundary values, workspace
component rules, limit and boolean refusal, and formatter round-trip/refusal.
The PowerPC main app built and was published fork-aware and exercised in
OS 9.2.2. Edit > Preferences… opened and seeded the compiled model, key,
workspace, 32/64 limits and the debug toggle; OK saved
`System Folder:Preferences:Sherclawk Preferences` and a quit/relaunch loaded
it again (2/1 limits, toggle checked). Malformed fields were refused in place:
a bad workspace and a non-numeric round count each showed the validation alert
with the dialog still open. With limits 2/1 a request paused after 1 round and
1 tool naming the configured ceilings, and Continue resumed. The debug toggle
then rendered each call as `• name(args)`, the result under `»` and its
`journal:` event names; the session JSONL kept only the normal records. The
workspace field set to `Retro68:PrefsTest:` applied immediately: the next
New Chat wrote its journal to `Retro68:PrefsTest:Sherclawk Sessions:` and
tools resolved against that root. Restoring `Retro68:` and 32/64 left a clean
state. Two fixes came out of the guest pass: `FSMakeFSSpec` returns `fnfErr`
for the not-yet-existing preferences file (first run and first save), and
`ModalDialog` reports a DITL checkbox hit without toggling it, so the dialog
tracks the toggle itself (`SetControlValue` followed by `Draw1Control`). The
saved key was present in the file and used by the runs, but not yet
distinguished from the identical compiled key in a no-compiled-key build.
