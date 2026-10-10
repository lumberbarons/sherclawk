# Historical verification record

Dated acceptance evidence for the OS 9 guest, host checks and live model runs,
retained from earlier development. Entries describe the state on their recorded
dates. Use the [current guides](../../README.md#documentation) for present behavior.

Recorded October 10, 2026 (display extraction, issue #145): Clang and Linux
GCC 13 ASan/UBSan host checks passed the extracted formatting, validation,
transcript capping and scroll/layout fixtures. `tools/check-transport.sh` and
`tools/lint.sh` passed. Docker `./build.sh` and `./build.sh all` built the app,
large-text target and both diagnostics. In OS 9.2.2 on the AFP `Retro68` volume,
`SherclawkScrollCheck.log` reported `RESULT failures=0` for focus/read-only
behavior, arrows, pages, thumb tracking, stale-control correction, clamps,
replacement/New Chat resets, wrapped 30,000-byte text and 1,500 separators.
`SherclawkHandoffCheck.log` also reported `RESULT failures=0`: unsupported
encoding and oversized summaries retained history, Markdown was saved and
verified, the journal switched, a second handoff used a distinct file, and
Continue resumed from the seed. The handoff diagnostic showed its PASS message
through the extracted transcript appender. These diagnostics used synthetic text
and summaries with no model requests.

Recorded October 10, 2026: `move_to_trash` was exercised in OS 9.2.2 against
the AFP `Retro68` volume. Real sessions had failed every move with `-43` because
`FSpCatMove` was given a file-named destination instead of the Trash directory
(the host stub had modelled a move-and-rename). After the fix,
`SherclawkTrashCheck` reported zero failures: `FindFolder(kTrashFolderType)`
resolved on the same volume, a pinned fixture moved with `mutation_intent` and
`mutation_committed` records, the source path was gone, the destination kept its
data size, type/creator and empty resource fork, and the item moved back out of
the Trash with its original bytes. A name already in the Trash is refused
`EXISTS` (`-48`). Not exercised: the workspace fallback Trash and a batch of
eight in the main app.

Recorded October 9, 2026 (large text, **host/build evidence only; guest pending**):
Clang and GCC 13 ASan/UBSan checks passed the 64 KiB read/edit and immutable
build-arena fixtures, all transfer/open/close/flush fault positions in a
multi-chunk edit, journal/rename faults, phase cancellation, tick wrap, JSON
page reconstruction, large search continuations and aggregate rejection before
queue creation. A separate shipping-gate test keeps normal tools at 4 KiB.
Transport checks, shellcheck and cppcheck passed; ruff was unavailable (no
Python source changes). Docker builds passed for the app, full-app large-text
acceptance target, edit diagnostic and build diagnostic. Linked app size was
561,152 text + 9,440 data + 2,922,856 bss = 3,493,448 bytes (3.33 MiB), excluding
heap/stack. The partition requests 6 MiB minimum / 8 MiB preferred; the starter
is unchanged. Native phase timings, maximum step duration, heap headroom,
visible Stop/responsiveness and a model-driven search/read/edit/read/build
have **not** been measured: no AFP host/guest connection was configured in the
implementation workspace. The shipping cap remains 4 KiB. See
[the pending acceptance procedure](../large-text.md).

Recorded October 9, 2026: `view_image` was exercised in OS 9.2.2. The
`SherclawkViewImageCheck` diagnostic reported zero failures against the real
File Manager: a PNG at the 128 KiB cap read in eight bounded steps and handed
over byte for byte, an over-cap file, a non-PNG and a missing file refused with
distinct codes, and a "no" or unknown vision flag refused without reading the
file. (Its first run caught `131072L` leaking into the error text and the model
schema from a stringified macro; fixed and covered by host assertions.) In the
main app, `openai/gpt-6-luna` was asked to look at a generated 240x160 PNG
of a red rectangle and a blue circle. It listed the folder, called
`view_image`, and described a red shape on the left and a blue circle on the
right against a light background. The session journal held the text note and
the tool result only, with no `base64` or `image_url`, and the following round
uploaded about 25 KB. Not exercised: a text-only model, a PNG near the cap sent
to a provider, and Stop during a read in the main app.

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
limits are in [macrelix-worker-verified.md](macrelix-worker-verified.md). Model-facing build/run
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
build IDs and compiler results are recorded in [macrelix-worker-verified.md](macrelix-worker-verified.md).
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
IDs and retained evidence are in [macrelix-worker-verified.md](macrelix-worker-verified.md).


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

Owned application Quit verification, October 9, 2026: `SherclawkQuitCheck`
reported `RESULT failures=0 project=BuildCheck000d104ds` in the running OS 9.2.2
UTM guest. The independent, responsive app accepted one noninteractive Quit
but kept running: `run-000d16c3-0001` (PSN `0:47185922`, build
`build-000d12df-0002`) returned `uncertain/QUIT_TIMEOUT`. A subsequent launch
returned `quit_supported:false`, `PROCESS_PREEXISTED`, new run
`run-000d1fbd-0002` and the original handle; Quit with the new run returned
`error/RUN_NOT_OWNED`. `SherclawkQuitIgnore.log` contained exactly one
`Quit received` line, confirming the pre-existing launch caused no additional
event. The generated starter from `build-000d2205-0004`, run
`run-000d25f8-0003`, returned `ok/QUIT_OBSERVED` through its ordinary cleanup
handler. The diagnostic pumped high-level events and updates while observing;
the guest continued processing input and advancing the diagnostic.

The first guest attempt safely withheld authority because the initial protected
process check rejected the default `ShCk` creator used by generated artifacts.
It sent no Quit. The correction protects Sherclawk by its exact current PSN,
while preserving artifact, launch-date, launcher, Finder and system exclusions.
Main app, template and Quit diagnostic Docker builds passed; clang ASan/UBSan
host suites and Linux GCC 13 ASan/UBSan suites passed. The successful guest
logs are retained locally under ignored `build/quit-check-verified.log` and
`build/quit-ignore-verified.log`. The schema/discovery gate is now open.

MCP Servers editor verification, October 9, 2026: the main `Sherclawk` build
(Docker, with the configuration editor) was published to the AFP share and run
in the OS 9.2.2 UTM guest. **Edit ▸ MCP Servers…** opened the dialog with the
disabled template and the no-file hint. Saving the template plus a stray `x`
showed `Invalid MCP configuration: JSON object.` and wrote nothing; removing
the `x` saved (first create) and the status line reported the save. Reopening
showed the saved text. Select All and typing replaced it with a Tavily-shaped
configuration (a placeholder key) followed by blank lines; the scroll bar
gained a thumb only once the text overflowed, auto-scrolled to the caret, and
page-up and page-down tracked with the thumb in step. Saving again (replace)
and reopening showed the new text. Cut emptied the pane and Paste restored it;
Cancel after edits discarded them on reopen. `Retro68:Sherclawk.log` held no MCP
or credential text. The file was then reset to the disabled template and the app
quit.

Not exercised in the guest: the 8 KiB limit messages, store failure branches
(covered by `tests/test_mcp_store.c` only), refusal while a run is active,
Enter/Escape key shortcuts, an application restart, and `SherclawkMCPCheck`
reading an editor-written file. A synthetic click needs a held press
(`TrackControl`) to scroll the pane. Host and guest results do not establish
the integrated memory partition or any agent use of the configuration.

## MCP Tavily diagnostic, October 10, 2026

Issue #159's native diagnostic gate passed in the OS 9.2.2 UTM guest against
the official Tavily endpoint. The owner had saved the private configuration
through **Edit ▸ MCP Servers…** before this session. The diagnostic read
`System Folder:Preferences:Sherclawk MCP Servers.json` directly; no host-written
configuration or credential-bearing response dump was used. No additional
trust anchor was needed.

The final diagnostic was Docker-built from source commit
`20c54ed8043c55436cce581dfb8fb2104ef0b0d9` and published with the usual AFP
fork metadata. Local `build/SherclawkMCPCheck.APPL` and the published app had
the same SHA-256:
`d5bf8bc5c6e9df8a54a3a489c6ff85c0697a955e15a9f54acfcb15182f88092d`.
After the guest was restarted, its log reported:

```text
PASS configuration
PASS TLS initialize version=2025-11-25 pages=1 entries=5 eligible=2
PASS Tavily search results=5 bytes=13470 (body deliberately omitted)
END PASS
```

The search gate now checks for HTTP/HTTPS result URLs, rather than treating
any non-empty content as successful search. It decodes into existing scratch
and logs only the count and message length. The five discovered entries
yielded two eligible tools; the diagnostic called `tavily_search` with its
fixed query, five results, basic depth and images/raw content disabled.

The first Escape attempt exposed a real Type 3 crash: initialization was in
the TLS 1.3 handshake, but `MacTLS_Close` tried to close BearSSL's uninitialized
T0 runner. `certainly-tls13-stop-handshake.patch` skips that runner during
the separate TLS 1.3 handshake and closes the transport directly. The
TLS 1.2 fallback still uses graceful close. Host transport tests assert both
paths against the actual patched Certainly source.

Two final-binary Escape runs then completed without that crash:

```text
STOP Escape phase=1 tls_state=1 pending_connect=1 request_id=1
PASS Stop drained ticks=3 contexts=0 request_id_unchanged=1 (server cancellation unproven)
FAIL stopped (cancellation best effort; no replay)
END FAIL

STOP Escape phase=1 tls_state=2 pending_connect=0 request_id=1
PASS Stop drained ticks=1 contexts=0 request_id_unchanged=1 (server cancellation unproven)
FAIL stopped (cancellation best effort; no replay)
END FAIL
```

Here phase 1 is initialization; TLS state 1 is connecting and state 2 is
handshaking. An intentional stop reports `END FAIL` because it did not finish
the search. Both runs reached terminal cleanup with no retained contexts and
an unchanged request ID. The host client fixtures cover no retry/replay; these
guest observations do not prove cancellation of work on the server. A third
run stopped an already connected initialization exchange and also drained
with zero contexts.

Editor acceptance used the existing guest `Sherclawk` app, SHA-256
`a9e7cfcb0f94c25561bc3743f91400460825818d4d1c706cdb4cde3771ad4ae3`.
After quitting and relaunching, opening the editor and pressing keypad Enter
saved successfully. Replacing the text with `x` produced the fixed JSON-object
validation alert when a save was attempted. After dismissing the alert, Escape
closed the dialog; reopening and saving retained the valid configuration.
The subsequent diagnostic passes, including after the VM restart, prove that
the editor-written file persisted and was consumed. While a model response
was active, Preferences and MCP Servers were visibly disabled in the Edit
menu; clicking MCP Servers opened no dialog. Command-Period stopped the
acceptance run afterward.

Credential-free logs and UI evidence are retained locally under ignored
`build/159-final-search-pass.log`, `build/159-final-connect-stop.log`,
`build/159-final-handshake-stop.log`, `build/159-art-busy-menu.png` and
`build/159-art-refusal.png`. The diagnostic omits bodies, header values and
session IDs. No credentials were committed.

`tools/check.sh`, `tools/check-transport.sh` and `tools/lint.sh` passed;
the transport suite also passed under Linux GCC in the Retro68 image.
Docker builds passed for `Sherclawk_APPL` and `SherclawkMCPCheck_APPL`.
Still not exercised in the guest: the editor's 8 KiB limit messages, store
failure branches (host fault tests only), Return-as-newline and Command-Period
inside the editor, or refusal specifically during a stopped lookup's drain.
This gate does not establish agent integration, credential redaction of
model-visible results, minimum-partition headroom or release acceptance.

Raised limits verification, October 10, 2026: the main `Sherclawk` build (Docker,
history 1 MiB, tool results 4096 bytes, prompt 8 KiB, `SIZE` 10 MiB minimum / 16 MiB
preferred) was published to the AFP share and run in the OS 9.2.2 UTM guest with
`openai/gpt-6-luna`. It launched in the new partition and the status line read
`History: 0/1024 KiB`. Reading `Minefield9:game.c` (4,083 bytes) took three pages
whose results were 2,190 and 2,344 bytes, which the old 1,536-byte cap could not
hold. A 4,352-byte prompt (built by select-all, copy and paste, since a longer
paste is refused with "Paste is empty or exceeds the field limit") was sent whole
and journaled at full length. Seven such prompts overflowed the transcript; its
top then read "[Earlier conversation is saved in the session file.]" followed by
the surviving recent entries, where the old code cleared the pane.

History was then grown with about 8 KB prompts, one model round each, every round
ending `end=ok`. Requests reached `up=1043436` bytes at 1002/1024 KiB (status
"History nearly full. Save Handoff (Command-H) to free h…"); the largest rounds
took about 3.7 to 5.4 seconds in all, the upload itself about one second
(`sent` minus `handshake`, in ticks), far inside the 120-second deadline. At 1022 KiB
the next send was refused with "Session/history unavailable. Start a new
session." and the prompt stayed in the field. Save Handoff at that size sent
`up=1046928`, saved the summary and left a fresh 2 KiB history. The app then quit
cleanly (`Sherclawk session ended.`).

Not measured: guest heap headroom (the main app log has no FreeMem line) and the
real iMac's network and CPU speed; the guest's network is QEMU user networking.
The model's context window (about 1M tokens by the status line) was never close
to the limit, so provider-side context overflow at 1 MiB was not exercised.

## MCP configuration guard, October 10, 2026

Issue #160's identity guard was checked in the OS 9.2.2 UTM guest against the
real Preferences folder and the owner's saved configuration. The workspace was
pointed at the boot volume so it contained `System Folder:Preferences`. The
diagnostic's log carries statuses, codes and identifiers only, never a result
body, and every hostile call ran without a journal (`move_to_trash`, which
checks for one first, ran with a journal and a revision that cannot match).

`SherclawkMCPGuardCheck` was Docker-built from source commit
`f07dd0a2905aaf43afdd0cc3d3588b7f0f25329c`. Local
`build/SherclawkMCPGuardCheck.APPL` and the published app had the same SHA-256
prefix, `2315b5db88ae9f5d`; the full local digest is
`2315b5db88ae9f5db35a9169c8043a7944b3d00d40f58fc52876fbdffacb1ad5`.

```text
NOTE FindFolder err=0 vref=-1 dir=45
NOTE spec vref=-1 parID=45
PASS tool specs and FindFolder agree on volume and directory
PASS guard recognises the configuration spec
...
PASS configuration size and modification date unchanged
RESULT failures=0
```

The run had 67 passing checks. Every protected tool answered
`PROTECTED` with `os_error=30001`, for the configuration, its `.new` and `.old`
siblings, and upper- and lower-case spellings of both. Those were `read_text`,
`get_file_info`, `list_resources`, `read_resource`, `resolve_alias`,
`write_text`, `create_folder`, `create_project`, `edit_text` and
`move_to_trash`. `list_files` paged over the folder without listing the
configuration, `search_text` skipped it (non-zero `skipped`), an alias made with
`NewAlias` to the configuration was refused by `resolve_alias`, and ordinary
files and alias targets in and around the same folder still worked. The
configuration's size and modification date were unchanged afterwards. The
guest confirms the assumption the host fixture cannot: the `vRefNum` and
`parID` that tool paths resolve to equal what `FindFolder` reports.

The first run showed `move_to_trash` stopping at `JOURNAL` because the
diagnostic passed no journal; the diagnostic was corrected and rerun. The
unchanged-tool diagnostics `SherclawkInspectCheck`, `SherclawkSearchCheck` and
`SherclawkWriteCheck` were rebuilt with the guard and each ended
`RESULT failures=0` on the `Retro68:` workspace.

Not exercised: `view_image` (host fixtures only), `build_project` inputs and
`run_application` against the real configuration, and the `Sherclawk
Preferences` file, which the guard does not cover (#170).

Recorded October 10, 2026: the `Retro68:Older:` and `Retro68:Spikes:` archives
named in the October 2 entries no longer exist on the AFP share; those entries
describe the state on their recorded dates.

## Abort mid-connect drain, October 10, 2026

Issue #34's gate: an abort that fires while a request is still connecting must
not tear the TLS context down mid-connect, because classic OT can fault on an
outstanding async connect. Every abort branch (lookup, send, handoff) and quit
now hand a live exchange to one bounded drain that closes it only once
`network_step` reaches a terminal state; Send, Save Handoff, Preferences and
MCP Servers report that the stopped request is still closing until it does.

`Sherclawk` was Docker-built from the branch carrying the shared drain and
published to the AFP share; local `build/Sherclawk.APPL` and the published app
had the same SHA-256,
`19e18d6178829ee1e454e55fa2cfafeae189d62ddcd3b4ee3dde3e766db714c4`.
The OS 9.2.2 UTM guest ran `openai/gpt-6-luna` with the owner's saved
Preferences. Command-Return sends and Command-Period stops; the tightest stops
were two QMP `send-key` chords 15 ms apart, so the abort could land at tick 1,
before the first TCP connect mark.

The session log's abort lines show the close landing at tick 4, 1 and 2 after
the round aborts, tick 4 after the handoff stop and tick 1 after quit. The
`connect=-` lines are the window where the connect had not yet been marked; the
close was deferred until it settled. The pre-fix crash was not re-run as a
control. These lines predate the pump-only drain (no request is sent while
draining; connected exchanges close at once), so that guest gate needs a rerun
before it is treated as covering the current code:

```text
round=1 init=0 connect=3 handshake=- sent=- first_byte=- done=- close=4 up=0 down=0 end=abort
round=1 init=0 connect=- handshake=- sent=- first_byte=- done=- close=1 up=0 down=0 end=abort
round=1 init=0 connect=- handshake=- sent=- first_byte=- done=- close=2 up=0 down=0 end=abort
handoff init=0 connect=- handshake=- sent=- first_byte=- done=- close=4 up=0 down=0 end=abort
round=1 init=0 connect=- handshake=- sent=- first_byte=- done=- close=1 up=0 down=0 end=abort
Agent stopped; completed records retained. Reason: Application quit.
Sherclawk session ended.
```

Every abort left the app running with the `Stopped.` transcript entry; a send
attempt during the drain was refused with `The stopped request is still
closing; send again in a moment.`; after the drain a later request completed
(`end=ok up=23093 down=5551`), and the handoff stop showed `Handoff stopped;
conversation retained.` with a following `end=ok` round. Quitting while a
request was still connecting drained and exited with `Sherclawk session ended.`
and no crash. `tools/check.sh`, `tools/check-transport.sh` and `tools/lint.sh`
passed on the branch.

Not exercised: the send-time lookup's own 30 s deadline can still close before
the transport timeout (filed as #182), and the pre-fix crash was not re-run as
a control — #34 records it at three type-3 failures.
