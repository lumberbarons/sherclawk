# Historical verification record

Dated acceptance evidence for the OS 9 guest, host checks and live model runs,
retained from earlier development. Entries describe the state on their recorded
dates. Use the [current guides](../../README.md#documentation) for present behavior.

Recorded October 21, 2026 (standalone guests, issue #68): the base image built from
a copy of the original QEMU disk (Mac OS 9.2.2, QEMU 10.1.2, `mac99`, 512 MB) plus the
MPW-GM distribution, with MacRelix absent. Both slot overlays booted headless side by
side to the desktop, auto-mounted the `Retro68` share and held independent AFP
sessions; `info network` showed MACs `52:54:00:09:00:01` and `52:54:00:09:00:02`,
each on its own user-mode stack at `10.0.2.15`. `SherclawkNativeBuildCheck`, published
with `tools/deploy-to-share.sh` and launched in the working image and then in each slot,
reported `RESULT failures=0 fixed_native_build=complete` every time, with MrC, PPCLink
and Rez at `stat=0` through ToolServer and no MacRelix in the logged process list.
Afterwards the base matched its recorded checksum, the verbatim copy matched the
original disk, and the original disk and the UTM virtual machine's image kept their
modification times. Procedure and configuration: [harness provisioning](../../harness/docs/provisioning.md).

Recorded October 16, 2026 (host cost counters, issue #19): `SherclawkCalibrateCheck`
built from the issue branch, published to the AFP share with `tools/deploy-to-share.sh`
and launched in the OS 9.2.2 UTM guest with the Retro68 window frontmost, the guest
otherwise idle. Each of three runs reported `RESULT failures=0` and deleted its
scratch file. The first run lacked the write, flush and create rows; the third ran
after the diagnostic was changed to stop on a failed open. Totals over 200
repetitions, as run one, two and three: open and close 138, 135, 146 ticks; seek and
1 KiB read 65, 66, 74; open, read and close 205, 201, 222; catalog lookup 69, 64, 74.
Seek and 1 KiB write took 75 and 86 ticks (runs two and three). Over 20 repetitions,
`FlushVol` took 7 and 9 ticks and create and delete 62 and 81. Over 300 event turns,
sleep 0 took 5, 5 and 41 ticks and sleep 1 took 331, 323 and 300; sleep 10 took 251,
254 and 257 ticks over 30 turns. The per-call ranges in [Cost table and guest
calibration](../development.md#cost-table-and-guest-calibration) come from these.

Recorded October 15, 2026 (model chooser and reasoning effort, issue #62
follow-up): OS 9.2.2 running the main application built from main (`0f99919`
was the head). With DHCP DNS in place, Edit > Preferences loaded the popular
catalog, Cancel closed it and left the app usable, and a model request completed
(`end=ok`, no `TLS error`). That is the successful baseline the October 14 entry
lacked; the `OT=-3162` failures did not recur, so #234 is not reproduced on this
build.

The DNS-override fault injection was then repeated and did not produce a failed
fetch. The guest's name server list was given `192.0.2.1` in Advanced mode
(DHCP, IP address, subnet and router unchanged). Applying it closed the AFP
connection and OS 9 quit the running Sherclawk, since the application runs from
the share. After remounting `Retro68` from Recent Servers and relaunching,
Preferences still loaded the catalog, and it did so again after a guest restart
with a cold DNS cache. The override persisted across the restart in the TCP/IP
panel, so it appears to be additive to the DHCP-supplied server (`10.0.2.3`)
rather than a replacement; the cause was not isolated. This means the October 14
"Could not load the catalog" observation is not explained by the override alone.
DHCP DNS was restored afterwards (Basic mode, name server `10.0.2.3`, IP address
`10.0.2.15` unchanged) and a prompt completed again.

Host-side notes from the run: a dropped AFP connection leaves an `afpd` holding
the application, its `._` sidecar and the session journal, and the next launch
fails with "could not be opened, because it is in use". Only that stale session
was ended; netatalk and the live mount were left alone. Special > Restart again
stopped at a black firmware screen and the VM was power-cycled through UTM.

Not verified: recovery after a failed or offline catalog fetch and Cancel of a
fetch known to be in flight. The guest's workspace is on the network volume, so
the network cannot be cut without losing the application, and the DNS override
does not fail the fetch. The API key appears in clear text in Preferences
screenshots; none were retained.

Recorded October 14, 2026 (model chooser and reasoning effort, issue #62):
the October 13 guest evidence on issues #62 and #57 covers popular-model loading,
search, rejection of a nonexistent typed model ID, exact confirmation and saving
of an existing typed ID, model-specific effort choices/defaults, persistence of
model and effort across relaunch, and provider requests at `low` and `max` with
different logged reasoning-token counts. That run also passed BuildCheck,
RunCheck, QuitCheck and SelfBuildCheck (`RESULT failures=0`). These are prior
observations, not a rerun of those four diagnostics on the October 14 build.

The remaining failure/Cancel checks used OS 9.2.2 and the main application built
from `58a76bd`, published with `tools/deploy-to-share.sh`. With permission, the
guest's DHCP configuration was given an unreachable DNS override (`192.0.2.1`),
leaving its IP address, subnet and router unchanged. Applying the override closed
the existing AFP connection; the `Retro68` workspace was remounted by server IP
without restarting netatalk. Preferences then displayed "Could not load the
catalog. Try Find again." with the saved model and effort still visible. Find
started another search, and Cancel returned to the main window without a crash.
After clearing the override, the TCP/IP panel again showed the original DHCP
DNS server (`10.0.2.3`) in Basic mode, and AFP was remounted. A separate open
captured "Loading the popular models..." immediately before Escape closed the
dialog. This is visible fetch/Cancel evidence; internal drain state was not
instrumented.

Recovery verification is blocked by #234: a subsequent model request and a fresh
catalog load failed after the DNS settings were restored; the model log recorded
`TLS error: code=4 bssl=54 OT=-3162`. The same request failure persisted after an
authorized guest restart, completed by stopping/starting UTM when the normal
Restart remained at a black firmware screen. AFP mounted again after boot. The
request log showed `up=0 down=0` and no handshake/send completion; the SDK names
`-3162` as `kOTNoDataErr` (TNODATA). Host DNS and the public catalog worked.
**No successful guest catalog/model baseline was taken on this build before
changing DNS.** Neither the failed fetch nor the persistent transport error can
therefore be attributed to the DNS override. The previous successful guest run
used a different revision (`16c1888`). This entry does not establish a successful
post-failure request or sign off issue #62.

The earlier display findings stay separate: #213 (disabled Effort text) is
closed, and #214 (cached-list drawing on the main window) remains tracked. No
application code was changed for this verification. `tools/check.sh`,
`tools/check-transport.sh`, `tools/lint.sh` and the Docker app build passed.

Recorded October 10, 2026: `copy_file` was exercised in OS 9.2.2 against the
AFP `Retro68` volume. `SherclawkCopyCheck` reported zero failures: a native
fixture with both forks (3,000 data and 1,500 resource bytes) and Finder
identity (`APPL`/`ShCk`, custom-icon flag set) was pinned through
`get_file_info` and published under a new name with `mutation_intent`,
`mutation_staged` and `mutation_committed` records, returning `COPIED` with
the destination's own `cat-` revision. The destination preserved both fork
sizes, type/creator and flags without the lock and alias bits, every byte of
both forks read back identical, and the source stayed untouched. A second
copy onto the published name was refused `EXISTS` before any I/O, with no
journal records. Not exercised: a copy at the 1 MiB cap, Stop mid-copy and a
publish from a real build artifact in the main app.

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

## MCP resumable processing, October 10, 2026

Issue #161's guest gate: `SherclawkMCPCheck` was Docker-built from source
commit `d15752c8379bcbf74cf4cf89f0b4b5660ab775f1`, published with AFP fork metadata and launched in the OS 9.2.2
UTM guest against the official Tavily endpoint with the owner's saved
configuration. Message parsing, key checks, discovery and result checks ran in
8 KiB steps; the SHA-256 of the published app was
`e29c868af73c9c62f0e68e2cce571b2e19f5757500406ed8e485497451ff57e3`. Its log:

```text
PASS configuration
PASS TLS initialize version=2025-11-25 pages=1 entries=5 eligible=2
PASS Tavily search results=5 bytes=13470 (body deliberately omitted)
END PASS
```

Escape during processing and a pending connect are covered by host fault
tests only (`tests/test_mcp_client.c`); no guest Stop run was repeated for this
change.

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
draining; connected exchanges close at once), so they are superseded by the rerun
below:

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

Rerun with the pump-only drain (October 10, 2026, OS 9.2.2 UTM guest, same
model and Preferences). Docker-built `Sherclawk` published to the AFP share;
local and published SHA-256 matched,
`a6717010e752886e101687978a223d2f430db21aa8e70da47c2fd1ec0f11bb43`. Stops were
a QMP `send-key` Command-Return then Command-Period about 15 ms apart; the
handoff stop clicked Save Handoff and sent Command-Period straight after the
button release. No abort line below carries a `sent` tick or request bytes
except the completed round:

```text
Model context lookup stopped. Reason: Stopped. Completed tool results are retained.
round=1 init=0 connect=3 handshake=- sent=- first_byte=- done=- close=5 up=0 down=0 end=abort
handoff init=0 connect=2 handshake=- sent=- first_byte=- done=- close=4 up=0 down=0 end=abort
handoff init=0 connect=1 handshake=- sent=- first_byte=- done=- close=4 up=0 down=0 end=abort
handoff init=0 connect=2 handshake=- sent=- first_byte=- done=- close=5 up=0 down=0 end=abort
round=1 init=0 connect=3 handshake=15 sent=15 first_byte=57 done=121 close=144 up=19799 down=3725 out=16/6000 reasoning=9 end=ok
round=1 init=0 connect=3 handshake=- sent=- first_byte=- done=- close=13 up=0 down=0 end=abort
Agent stopped; completed records retained. Reason: Application quit.
Sherclawk session ended.
```

The app stayed running after every stop, a later request completed, and
Command-Q during a connecting request exited to the Finder with no crash.
Three earlier handoff stops landed after the connect (`handshake` and `sent`
set, `up=1133`) and closed at once, as designed for a connected exchange. Not
exercised: the 30 s forced-close backstop and a long hidden-window quit wait.

## MCP run loop in the guest, October 11, 2026

The app built from the integration branch was published to the AFP `Retro68`
volume and driven in the OS 9.2.2 UTM guest with the owner's saved MCP
configuration (Tavily) and a live model. It found two defects the host
fixtures could not:

- Discovery succeeded but the freeze refused the real Tavily schemas
  (4,129 bytes for the two eligible tools): the native rows leave about
  1.7 KB of the 16 KiB merged buffer, far below the nominal remote cap. The
  merged buffer is now 24 KiB, and a host test freezes a set exactly at the
  cap.
- The app exited silently at the first remote call. Open Transport is closed
  between model rounds (an idle client holds no endpoint), and only discovery
  reopened it, so the call created a TLS context with Open Transport closed.
  The controller now opens it before every call, asserted by the host
  fixture.

With both fixed, one prompt asking the model to search the web produced, in
order: discovery admitted both tools (`mcp_discovery`), a model call to
`mcp_tavily_tavily_search` with the model's arguments verbatim, a retained
result (`mcp_result_retained`, 4,744 bytes, one item, source links in the
summary), a `read_mcp_result` page, and a final answer with a source link.
Stop pressed while the call was in flight recorded a `STOPPED` result (outcome
unknown, not replayed), journaled the stop notice and left the app running;
the next prompt re-discovered, searched and retained a second result with a
session-scoped ID. This passed with the partition set to the 10 MiB minimum
and at the 16 MiB preferred size.

Memory, read with temporary `FreeMem`/`MaxBlock`/`StackSpace`/zone logging
(not shipped): the application zone measured 10.74 MiB at the 10 MiB setting
and 16.74 MiB at 16 MiB, and the system had roughly 480 MiB of temporary
memory free. Toolbox free heap was about 118 KB at rest in both, as it was in
guest sessions from before the integration, and fell to between 0.4 and
1.2 KB whenever a TLS exchange was open (the lowest seen was 400 bytes during
a remote call; model rounds alone reached 768 bytes), then recovered. Stack
space at the remote call was about 35.7 KB. The free-heap figure did not grow
with the partition, so a larger partition does not by itself add headroom to
that region; whether it is the true limit has not been established (it is not:
see the next section). Not
exercised: extraction and multi-page results, failed discovery against a live
server, quit during a call and repeated long sessions.

## Application heap headroom, October 11, 2026

A temporary probe (not shipped) logged the application zone, its limit,
`FreeMem`, `MaxBlock`, `MaxMem`, `StackSpace` and the addresses of `gNet`,
`gAgent` and the code to `Retro68:Sherclawk.log`. The app was built from
`main` at `73a3d3b`, published to the AFP share and launched in the OS 9.2.2
UTM guest by keyboard over QMP; one prompt asked the live model to call
`get_environment`.

At the 16 MiB preferred size, at entry to `main`, the zone had grown to
6,979,344 bytes with 3,888 free and 10,574,064 bytes still below the
application limit; `gNet` and `gAgent` lay inside it and the code above the
limit. After window setup it was 7,005,008 bytes with 10,016 free. Calling
`MaxApplZone` then grew it to 17,553,392 bytes with 10,558,400 free, and with
the model's TLS context open `FreeMem` read 10,440,608. So the 118 KB figure
in the previous section was the slack of a heap the Memory Manager grows on
demand, not the heap's capacity.

With `MaxApplZone` at the top of `main` (as shipped) and `SIZE` temporarily set
to 10 MiB preferred, the zone was 11,261,824 bytes, `FreeMem` read 4,267,344
after setup and 4,149,520 with a TLS context open, and `get_environment`
reported `free_heap_bytes` 4,266,608 to the model. Both runs completed their two
model rounds normally. The shipped build (16 MiB preferred) was republished
afterwards. A failed TLS context allocation was not induced in the guest; host
tests inject it for the model request and for MCP discovery and calls.

## Preferences crash from MaxApplZone, October 12, 2026

Edit ▸ Preferences… died in OS 9.2.2 with "an error of type 2" inside
`DialogSelect`, on the dialog's first activate event, in every build from
`68ad400` (#199) on. Bisecting on the guest: `a03b99c`, `db0a6c4`, `468efb9` and
`73a3d3b` opened the dialog; `68ad400` crashed; the same tree without its
`MaxApplZone()` call at the top of `main` opened it. The crash did not depend on
the catalog fetch or the Effort menu, the MCP Servers dialog opened normally
with the zone grown, and reserving 1 MB below the application limit with
`SetApplLimit` (`StackSpace` read 1,085,952) did not prevent it, so the stack
was not the cause. #199 had verified `MaxApplZone` after window setup and only
ran model rounds.

`MaxApplZone` was removed. `get_environment` now reports `FreeMem` plus the gap
between `LMGetHeapEnd` and `GetApplLimit`. Published to the AFP share and
launched by keyboard over QMP: Preferences opened with the popular models
listed and Cancel closed it, and a prompt that called `get_environment` returned
`free_heap_bytes` 10,534,960 at the 16 MiB preferred size, in line with the
10,440,608 to 10,558,400 measured with the zone grown. The guest had to be
force-stopped and restarted after repeated faults wedged it.

## Build-check fixtures, October 12, 2026

`SherclawkBuildCheck`, `SherclawkRunCheck`, `SherclawkQuitCheck` and
`SherclawkSelfBuildCheck` had failed (2, 3, 3 and 3) on `main` since #91 gave the
starter five sources: the fixture's edit of `"sources":["main.c"]` in the
starter's `project.json` no longer matched, so `extra.c` never joined the build,
the build meant to fail on its `#error` succeeded and the Run and Quit variants
launched an app they expected to be refused. The failures reproduced on
`3924cf1` in OS 9.2.2. The edit now inserts `extra.c` after `main.c`; all four
diagnostics report `RESULT failures=0` in the guest. In the Quit variant the
first project's `QUIT_TIMEOUT`, the `PROCESS_PREEXISTED` relaunch and
`RUN_NOT_OWNED` are expected outcomes, and the second project reports
`QUIT_OBSERVED`.

## Integrated MCP agent acceptance, October 14, 2026

Issue #165's release gate passed in the OS 9.2.2 UTM guest with
`SherclawkMCPAgentCheck`, built from the production UI, agent and MCP
controller on the branch based at `081707b`. The owner’s private Tavily
configuration was saved through **Edit ▸ MCP Servers…**; the diagnostic read
that file and the saved model preferences (`openai/gpt-6.1-sol`, medium).
It was published fork-aware to `Retro68:` on the AFP share and launched with
Command-O over QMP. AFP/netatalk was not restarted. No populated configuration,
key, session ID, schema or response body is included in this record.

`Retro68:SherclawkMCPAgentCheck.log` ended with `RESULT failures=0`:

| Acceptance case | Observed outcome |
|---|---|
| Real model-selected search and extraction | `s0001eed1.jsonl` records `mcp_tavily_tavily_search` and `mcp_tavily_tavily_extract`, both `status: ok`, `is_error: false`; retained bodies were 2,600 and 1,260 bytes. The final answer contained source links. |
| Retained pagination through the agent | The model read the entire extraction at offsets 0, 512 and 1,024 (end 1,260), and search at 0, 512, 1,024, 1,536, 2,048 and 2,560 (end 2,600), all successfully. Extraction used the small public IANA example-domains page. |
| Frozen configuration / disabled next run | After the private configuration was loaded, the diagnostic loader was changed to return a disabled fixture. The current run still discovered and used both tools; the next run called `get_environment` successfully without opening MCP transport. The private file was preserved. |
| Failed discovery | A public missing-endpoint fixture produced a sanitized notice, then the live model called native `get_environment` successfully (`s0001fb1f.jsonl`). |
| Stop and no replay | Discovery, remote call, received-message parsing, artifact publication and next-page dispatch were each reached and stopped. The frozen names cleared, cleanup settled and the RPC ID never advanced during drain. Call/parse/publication journals have `STOPPED`; the queued page has an interrupted result. |
| Credential/session checks | Exact private-value oracles passed on frozen schemas, pure model request JSON, history, final reply, retained artifacts, real model-generated handoff, closed session journals and lifecycle log. Host fixtures separately injected escaped and nested reflections. |

Stop-handler durations, measured in 60 Hz ticks, were 23 during discovery,
26 during the remote call, 26 during parsing, 25 during publication and 3
before the next page. The longest observed `DriveChatStep` was 34 ticks.
A retained page is a synchronous bounded 512-byte operation: this exercises
Stop between page dispatches, not interruption inside a File Manager call.
The server-side outcome of a stopped request remains unknown; no server-side
cancellation guarantee is inferred.

Both diagnostic `SIZE` values were 10,485,760 bytes; Process Manager reported
11,307,620 bytes including PEF loader overhead. Sampled heap headroom was
4,204,512 bytes after setup, 4,085,728 with a model TLS context open, and
3,978,640 with an additional unpumped model TLS allocation held while the MCP
connect drained. That stress allocation sent no model request. The final
headroom recovered to 4,203,760 bytes; the lowest sampled stack space was
38,400 bytes. These are observations between work steps, not a stack peak
measurement. The diagnostic contains 83,776 more linked bytes than the
shipping app, so this acceptance includes its additional storage.

Retro68 `powerpc-apple-macos-size` measured:

| Target | Text | Data | Bss | Total |
|---|---:|---:|---:|---:|
| `Sherclawk` | 651,264 | 10,672 | 6,906,256 | 7,568,192 |
| `SherclawkMCPAgentCheck` | 675,840 | 10,848 | 6,965,280 | 7,651,968 |

The diagnostic APPL's SHA-256 was
`1bde5bd1cbfab2c7df48e4573cd6de8e69880f569c143160570170b223cba77f`.
`tools/check.sh`, `tools/check-transport.sh`, `tools/lint.sh` (all three
analyzers installed), and Docker `./build.sh` for both targets passed.
No runtime buffer, deadline or shipping partition limit was raised. This
completes the integrated read-only MCP admission gate under ADR-0003; it
does not admit additional servers, tools or encodings beyond the existing
configuration, schema and redaction contracts.

The first diagnostic iteration correctly refused the preferred-size launch.
A subsequent run exercised search and Stop but extraction of a larger
reference page failed with the client's connection/HTTP-framing error; its
model read the retained search instead. That run was not counted as extraction
acceptance. Its journal scans also failed because the files were still open
for writing; the final diagnostic scans them after close. The final run above
requires successful retained extraction and distinct successful page reads.

## Capabilities and volume info, October 15, 2026

`get_capabilities` and `get_volume_info` were verified in the OS 9.2.2 guest
with `SherclawkInspectCheck` (extended for them) before either was added to
`agent_tool_schemas`. The diagnostic APPL's SHA-256 was
`c1fd176eedb27cd5251876193e65f8e809039f4d72d667afc1f5909f023b35bd`. Result:
`RESULT failures=0`.

`get_capabilities` reported a PowerPC 750 (`0x0108`), 536,870,912 physical and
546,541,568 logical bytes, and Color QuickDraw `0x251`, QuickTime 5.0.2, Open
Transport 2.7.9, Sound with input, Speech Manager, AppleScript 1.6.0 (Gestalt
`0x01100160`), Appearance Manager 1.1.1, Drag Manager, Thread Manager, Speech
Recognition 2.0.3 and QuickDraw 3D present. QuickDraw GX is not installed in
this guest: its Gestalt selector answered `-5551` and the tool reported
`"quickdraw_gx":{"available":false}` with no `error` field, while every other
feature still reported. The diagnostic reads Gestalt itself and compares each feature's `available`,
raw `value`, version value and (for Sound) input bit, plus the CPU type and
both RAM sizes, against what the tool reported, so every present and absent
answer above was checked, not just read. It logs a SKIP instead of a pass when
no selector is absent; here one is. The report was about 1,200 bytes of the
4,096-byte result cap.

`get_volume_info` paged the guest's volumes one per call (`limit` 1), each
page holding exactly one entry: Macintosh HD (8,588,574,720 total) and the
share Retro68 (105,089,261,568 total, writable), with `workspace:true` only on
the volume whose vref equals the workspace's. The Retro68 total exceeds 4 GB,
exercising the 64-bit counts; each entry's total bytes, free bytes (within
16 MiB, since files are written during the run) and read-only flag were
compared with a direct `PBXGetVolInfoSync` call, and the paged volumes matched
one `limit` 8 page in order. No volume in this guest is locked, so the
diagnostic logged a SKIP for the locked branch; the lock bits are covered by
the host suite only. A `path` argument was refused `ARGUMENTS`.

`tools/check.sh`, `tools/check-transport.sh`, `tools/lint.sh` and Docker
`./build.sh` passed. No runtime buffer, deadline or shipping partition limit
changed.
