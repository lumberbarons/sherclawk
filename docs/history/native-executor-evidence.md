# Native executor guest evidence

Detailed guest evidence for the ToolServer channel and the native build
executor, carried over from the retired build-queue design note. Decisions are
recorded in [ADR-0001](../adr/0001-execute-builds-natively-through-toolserver.md);
the summary is in [verification history](verification.md). File and line
references below describe the tree at the time of writing.

## Increment 1 guest evidence — October 6, 2026

Implemented `tools/toolserver-check.c` and its separate Apple-event-aware
`SIZE` resource, registered as `SherclawkToolServerCheck_APPL`. PowerPC build and
fork-aware AFP publication passed. The final diagnostic ran in OS 9.2.2 and
reported `RESULT failures=0 success_and_error_suite=complete`, then repeated that
result after an explicit ToolServer relaunch. Retained fixture:
`Retro68:ToolServerCheck004f2916:`. Raw evidence is ignored local
`build/toolserver-check-verified.log`, plus final/background screenshots.

Findings:

- ToolServer absent at launch: desktop-database discovery found `ToolServer` on
  volume `-1`, parent `4557`, and `LaunchApplication` succeeded without switching
  the diagnostic out of the foreground. The first queued send returned in one
  tick; later sends returned in zero ticks at the guest clock's resolution.
- Successful native Rez returned raw `stat=0`, empty stdout/diagnostics, and the
  resource fork contained exactly `STR `/128 = `native async lobster`. Deliberate
  syntax failure returned raw `stat=2` and the expected filename/line diagnostic.
  The raw failure corresponds to the documented `tlsrvr` mapping to exit 1;
  this spike preserves raw status and does not implement the production mapper.
  The earlier shell path's `'s must occur in pairs` warning did not appear.
- Reply correlation checked both return ID and sender PSN. Synthetic local
  guest events through the same handler rejected wrong IDs/senders, missing
  status, wrong text type and 8,193-byte text; `reply_checks failures=0`.
- A 41-Rez script kept processing events. Stop (ID 103) and deadline (ID 104)
  each recorded unknown without resend. Their late successful replies were
  drained with `abandoned=1`; they did not advance a build. Foreground runs
  completed in 727/676 ticks with 84/79 event-loop turns and maximum gaps of
  18/13 ticks respectively.
- Background run (ID 105) received `stat=0` without reactivation. It took 467
  ticks and only seven turns, with a maximum gap of 303 ticks (about five
  seconds). Background liveness is demonstrated; responsive latency is not.
- Polite quit during ID 106 was deferred: a successful reply arrived after
  690 ticks, still abandoned/unknown. An explicit `R` then found ToolServer
  absent, relaunched it and passed the success/error suite with IDs 108/109.
  An earlier run exposed sender coercion error `-1700` after ToolServer quit;
  the final code rejects unverifiable senders and retires outstanding requests
  as unknown when the server is observed gone. Abrupt process death and window
  dragging during reply delivery have not been exercised.

The first fixture revision lacked a Rez type declaration and correctly failed
both commands. Final sources define the type locally, and the successful output
is verified with the Resource Manager. Earlier fixtures/logs remain retained.
No MacRelix code is copied or linked into the diagnostic. No queue executor,
full native C build, automatic cancellation, or Preferences work is included.
Increment 2 uses this verified channel; its acceptance is recorded below.

## Increment 2 guest evidence — October 6, 2026

Implemented `toolserver.c`/`.h` as a single-outstanding-command asynchronous
client and `tools/native-build-check.c` as the fixed structured executor fixture.
The increment-1 spike stays independent. The diagnostic embeds the current real
starter sources from `build/project-template.h`, creates a fresh retained folder,
verifies exact CR/TEXT inputs, and executes three fixed steps: MrC, PPCLink, Rez.
It sets Finder `APPL/SHTP`, verifies a PowerPC PEF header and nonempty forks,
reads `cfrg`/0 and the exact 1 MiB `SIZE`/-1 allocations, writes/read-backs the
same `artifact=Template\n` success record, and launches the exact FSSpec through
the Process Manager. This is a fixed diagnostic, not descriptor/queue integration.

PowerPC builds and fork-aware publication passed for `SherclawkNativeBuildCheck`
and `SherclawkNativeBuildErrorCheck`. Both ran in OS 9.2.2. The first attempt
refused execution because a background MacRelix process existed; it was quit
normally before acceptance. Native process scans confirmed MacRelix absent before
each tool command and before launch. ToolServer was already running and idle.

- Successful fixtures `Retro68:NativeBuildCheck004fdbc6:` and
  `Retro68:NativeBuildCheck004fec51:` returned raw status 0 for compile, link
  and resource append, and `RESULT failures=0 fixed_native_build=complete`.
  Each artifact had 2,076 data bytes, 428 resource bytes, `APPL/SHTP`, an 84-byte
  `cfrg` and a 10-byte `SIZE`. Both exact artifacts were observed as native
  processes and displayed the expected title and both text lines; Command-Q
  quit them. First/repeat runs had 22/21 event-loop turns and maximum gaps of
  39/47 ticks. Sends took 5–7 ticks including process discovery/logging.
- Deliberate compiler-error fixture `Retro68:NativeBuildCheck004fe76c:` returned
  raw MrC status 1 and the expected filename/line diagnostic. It reported
  `RESULT failures=0 expected_compiler_failure=1 no_artifact=1 no_success=1
  no_launch=1`; no linker or Rez command was sent.
- Stop fixture `Retro68:NativeBuildCheck00503359:` stopped observation during
  PPCLink (ID 102). It recorded an unknown outcome, drained the late status-0
  reply with `abandoned=1`, and sent no Rez command or launch. The partial
  application remains for evidence, without a success record. Stop does not
  prove cancellation and nothing was resubmitted.
- Repeat source bytes and compiled resource payloads (`cfrg`, `SIZE`) matched
  exactly. Complete fork bytes were **not identical**: the PEF differed only
  at its timestamp byte 19; resource maps also differed in bookkeeping bytes.
  No byte-identical determinism is claimed. The old shell `good08` baseline
  contains the earlier 1,671-byte source, while the current close-box-aware
  starter has 1,922 bytes, so it is not a same-input binary comparison.

Raw evidence is ignored local `build/native-build-check-verified.log`,
`native-build-repeat-verified.log`, `native-build-error-check-verified.log`,
`native-build-stop-verified.log`, and `native-template-launched.png`,
`native-template-repeat.png`, `native-error.png`, `native-stop.png`. Fixtures
stay on the mounted AFP volume. No AFP restart or disk-image change was needed.

The client retains return-ID/sender correlation and bounded text extraction,
keeps abandoned requests outstanding until drained/server disappearance, and
never automatically retries. The fixed diagnostic has no worker lock, queue
scan, descriptor adapter, persisted build authorization, or model-facing tool.
Those were added in increment 3 below; the Perl worker remains a fallback.
Production status mapping, background/tracking latency guarantees, lost-reply
recovery and generalized executor ownership remain separate work.

## Increment 3 guest evidence — October 6, 2026

Implemented `selfbuild.c`/`.h`, a current-snapshot executor integrated with
`build_project`, plus the shared `BuildPlan` descriptor/command representation.
The main application installs the queued ToolServer handlers and dispatches
high-level events. It publishes existing snapshots without adding a ninth job
input, atomically acquires `worker-lock`, claims only its own fresh ready job,
revalidates and compares the snapshot, copies declared CR/TEXT sources, executes
one MPW command at a time, and writes compatible logs and protocol-1 results.
`started` stays unchanged; `native-executor` identifies native ownership.
Successful builds retain the existing persisted authorization for both forks.

ASan/UBSan checks pass, including ownership races, external lock/STOP fallback,
conflicting outputs, changed snapshots, send errors, status mapping, malformed
replies, Stop/deadline, late reply draining and successful artifact authorization.
The existing seven Python worker tests pass. Main and diagnostic PowerPC builds
pass. An old empty Perl lock was inspected with only Finder in the guest process
menu, then removed with explicit permission before native ownership acceptance;
no lock stealing is implemented.

`SherclawkSelfBuildCheck` ran in OS 9.2.2 and reported
`RESULT failures=0 project=BuildCheck000096d3s`. It scanned processes before all
six builds and recorded `MacRelix_absent=1`. Every snapshot retained its native
executor marker. Retained build IDs:

| Project / step | Build ID | Outcome |
|---|---|---|
| Independent deliberate error | `build-0000975b-0001` | Compiler exit 1; launch refused |
| Independent repaired | `build-000098fa-0002` | Success; authorized native launch |
| Independent fresh source revision | `build-00009b72-0003` | Success; new authority and launch |
| Starter deliberate error | `build-00009e12-0004` | Compiler exit 1; launch refused |
| Starter repaired | `build-00009f94-0005` | Success; authorized native launch |
| Starter fresh source revision | `build-0000a23d-0006` | Success; new authority and launch |

The independent project compiled two C files and one resource file with a
shared header/include path; starter builds compiled an added second source.
Each successful artifact passed PEF/fork/Finder checks and persisted authority;
the diagnostic read nonempty `cfrg`/0 (88 independent / 84 starter bytes) and
10-byte `SIZE`/-1 resources. `run_application` observed each exact artifact's
process. The independent windows displayed the expected original and edited
“Fresh revision compiled natively” text; Command-Q quit both normally. Broader
functional application testing remains separate from this build/launch acceptance. Error snapshots have no success
marker or launch authority. Logs retain per-command raw status, mapped exit,
stdout and diagnostics with the existing continuation contract.

`SherclawkSelfBuildStopCheck` also reported `RESULT failures=0` for
`build-00013dab-0001` with MacRelix absent. It stopped observation during the first
MrC command, returned uncertainty, and serviced events independently until the
late reply drained. `native-drained` retained `raw_status=0`, `abandoned=1`,
`malformed=0`; stdout contains only prepare and first-compile stage markers.
There is no second compile, linker, Rez, terminal result, success marker, launch
authority or launch. The lock was released after the reply drained; the claim and
`native-unknown` remain. This does not prove compiler cancellation.

Raw evidence is ignored local `build/selfbuild-check-verified.log` and
`selfbuild-stop-verified.log`, plus independent window and Stop screenshots.
Fixtures and all snapshots remain on the mounted AFP volume. No AFP restart,
VM flag change or disk-image deployment was needed. Queue creation, scanning
external jobs, preferences and queue status/toggle UI remain increments 4–5.
Background/window-tracking latency guarantees and abrupt ToolServer death remain
unverified in the integrated path. Unknown jobs never replay; quitting with an
undrained command deliberately retains the lock for inspection.
