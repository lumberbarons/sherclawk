# 005 — Sherclawk as its own build-queue worker

**Status:** ToolServer diagnostic, fixed native build and integrated self-build fixtures guest-verified; general queue service remains planned. Feasibility checked against the current source tree, the locally installed, user-supplied Universal Interfaces, and the upstream ToolServer client
that this workspace already drives via `tlsrvr`. Guest evidence for increments 1–3 is recorded below; general queue service
still needs its own implementation and acceptance.

## The idea

Run the same `Sherclawk` application in two roles:

- **Harness (today's role):** chat window, agent loop, tools, and
  `build_project` publishing jobs.
- **Queue worker (`sherclawk --queue` in Unix terms):** the app itself owns the
  build queue — creates it, claims jobs, executes them, records results — so no
  separate `worker05.pl` to publish, no MacRelix session to foreground, and no
  ssh step to create the folder.

The interesting conclusion from reading the code: **the queue bookkeeping is
already native; the only thing standing between the app and running its own
builds is the executor.** Fold the executor in and the queue worker falls out
almost for free, because `jobs.c` already speaks every file operation the job
protocol uses.

## What the pipeline does today

- `build_project` validates a protocol-2 descriptor, generates a trusted LF
  shell recipe (`mpw-ppc-v2`: MrC → PPCLink → Rez through
  `/Developer/Tools/tlsrvr`), publishes a protocol-1 job folder into
  `Retro68:Worker01:buildjobs` with `jobs.c`, and polls `result` plus separate
  stdout/stderr pages (`build_project.c:16,171-196,301-316`; `jobs.c:65-223`).
- `worker.pl` runs **in MacRelix**: mkdir `worker-lock`, scan for `ready`,
  rename to `claimed`, record `started`, run
  `exec /bin/sh script < /dev/null > stdout 2> stderr`, record `result`
  (`worker/worker.pl:7-80`).
- The queue must be created out of band with
  `ssh "$SHARE_HOST" 'install -d ... /srv/retro68/Worker01/buildjobs'`, and a
  human must start the worker in MacRelix and keep it foregrounded enough to
  poll (`README.md:178-188`) — an idle background worker may need activation
  to resume, and a lock is explicitly not liveness evidence
  (`worker/README.md:90-111`).
- Replacing the worker means publishing the Perl data fork under a fresh
  filename each time, because of observed AFP caching (`worker/README.md:92`).
- The one thing the app cannot do natively is execute that `script`: it is
  shell text whose real work is `tlsrvr` (a MacRelix tool) sending a `dosc`
  ("Do Script") Apple event to MPW's ToolServer. The research document already
  ruled out driving MacRelix by Apple event: Genie's `execute` handler was
  deleted upstream and is a trap (`docs/sherclawk-agent-harness-research.md:741-748`).

## Is `sherclawk --queue` possible?

Not as literal Unix argv. A Finder-launched classic application has no
command line; Retro68's `_start` passes `argc = 1, argv = {"./a.out"}`
(`Retro68/libretro/start.c:31-41`). The platform's real equivalents, best
first:

1. **A service inside the running app.** Queue processing is one more
   cooperative stepper in the existing `WaitNextEvent` loop — exactly how
   `DriveChatStep` already interleaves model rounds, tool dispatch and
   `build_project_step` (`main.c:1055-1081`). A File-menu toggle ("Serve Build
   Queue") plus a queue status window gives both modes in one process, no
   relaunch, and the app is frontmost so the background-polling starvation
   problem disappears. This is the most Mac-like answer to "two modes".
2. **Open-document launch.** Install an Apple Event handler for
   `kAEOpenDocuments` (`AppleEvents.h:87-90`): dropping the queue folder on
   Sherclawk in the Finder (or opening a small marker document) starts queue
   mode bound to that folder. This is the closest thing to passing an argument
   from the Finder, needs no new app, and is how droplets have always worked.
3. **Preferences (idea 002).** `queue_path`, `start_queue=1`, and the same
   macros for its path — persistent, discoverable, and already researched.
4. **A launch-time parameter from a companion droplet.** The Process Manager
   launch block has `launchAppParameters` ("Format for first AppleEvent to
   pass to new process", `Processes.h`), so a tiny launcher could start a
   second Sherclawk with a custom event carrying `queue` and a path. More
   machinery than the payoff justifies today; note it exists.

Recommendation: 1 + 2 + 3. Skip the droplet unless "double-click a
`Sherclawk Queue` document" turns out to be wanted; the marker document can be
added later without changing the engine.

## Can the app actually execute the jobs?

Queue semantics: yes, trivially — every operation in `worker.pl` is a File
Manager call the app already makes (exclusive folder create, marker rename,
closed-file records with read-back, `STOP` polling). `jobs.c` is the mirror
image of it.

Execution: that is the whole problem. A native app cannot run `/bin/sh` text,
and there is no supported way to make MacRelix run a command unattended. Two
answers:

- **Delegate forever.** The app folds in the queue loop but still needs a
  human-started MacRelix to execute anything. That removes no manual step;
  reject it.
- **Become the executor.** The recipes are already the app's own structured
  data (`build_project_recipe` is a pure function of the descriptor), and the
  only genuinely external operation is the MPW tool invocation. Implement the
  ToolServer Apple-event client natively and the app can run the same builds
  itself. This is the one that actually makes the process smooth.

### The ToolServer channel is smaller than it looks

`tlsrvr` is a thin client, and its protocol is fully visible in upstream
source (reference only — MacRelix is AGPL-3.0, so no code is copied; the
protocol is reimplemented from behavior and verified in the guest):

- Find ToolServer by signature `'MPSX'` through the Process Manager, or find
  its `APPL` through the desktop database and launch it with
  `LaunchApplication`.
- Build a MacRoman/CR script:
  `Set Exit 0; Directory <cwd>; <command> < Dev:Null; Set CommandStatus
  {Status}; Directory "{MPW}"; Exit {CommandStatus}` and send it as
  `kAEMiscStandards` / `kAEDoScript` with `keyDirectObject` as `typeChar`.
- The reply carries `'stat'` (`typeSInt32`), `'----'` (stdout text) and
  `'diag'` (diagnostics text). Status conventions: `stat == -1` means 127;
  result 2 from a non-cancelled run means 1; Command-period shows up as
  user-cancel text (map to 128); "Out of memory" triggers quit-and-retry-once.
- All pieces exist in the vendored interfaces: `AEInteraction.h` (AESend),
  `AEDataModel.h` (`typeChar`, `kAENoReply`/`kAEQueueReply`, param calls),
  `AERegistry.h:172,209` (`kAEDoScript`, `kAEMiscStandards`), `AppleEvents.h`
  (handlers, `oapp`/`odoc`/`quit`). Apple Event Manager functions link
  through the already-linked `InterfaceLib`.

Two implementation rules matter:

- **Send asynchronously.** `tlsrvr` blocks (`kAEWaitReply`); the app must not,
  or the UI freezes for the length of every MrC/Rez command. Use
  `kAEQueueReply` and receive the answer in the existing event loop
  (`kHighLevelEvent` → `AEProcessAppleEvent`), so `WaitNextEvent` keeps
  servicing the window while a compile runs. This is what makes the native
  client *better* than the shell path: real per-command statuses and
  diagnostics instead of one aggregate shell exit code.
- **Send without stealing focus** where possible (`kAENoInteract`), or accept
  the ToolServer focus hop and return focus to Sherclawk, the way `tlsrvr`
  deliberately does. The recorded `'s must occur in pairs` quirk
  (`research:764`) has to be reproduced or understood before claiming parity.

### What a native build executor looks like

Everything except the tool command is code the app already owns:

| Recipe step | Native implementation |
|---|---|
| `mkdir build/native` | `FSpDirCreate` (as `create_folder`, `tools.c:770-792`) |
| `cp` snapshot inputs | the inputs are already staged in the job folder by `jobs.c`; copy or compile in place |
| `SetFile -t TEXT -c ttxt` | Finder info write, already done by every create/edit tool |
| `tlsrvr -- MrC/PPCLink/Rez` | one async DoScript event each |
| `test -s`, `success.txt`, `echo` | catalog checks and plain writes the app already does in `build_project.c:255-270` |

Stage markers (`stage=compile started`, `stage=complete status=0`) and the
`stdout`/`stderr` job files can be written exactly as today, so `read_build_log`
continuation, the `success.txt` artifact record, and artifact verification stay
byte-for-byte unchanged for the model.

### Job format and coexistence

The app's jobs should stay protocol-1 folders — same queue, same journals,
same crash semantics — with one addition: alongside the compatible `script`,
write a structured recipe (or a manifest field naming the native executor).
`worker.pl` ignores the extra file and runs the script exactly as now; a native
worker claims only jobs it can execute and skips the rest. Either executor can
serve the compatible queue, but must first acquire the same singleton
`worker-lock`; initial integration uses exclusive ownership, not simultaneous
workers. The ready-to-claimed rename records ownership of each job. The two representations come
from the same generator, so they cannot drift. The `started` record should name
the claiming executor for later evidence (the app's strict result parser only
looks at `result`; check the Python worker tests before extending any record).

## The queue folder can create itself

Yes, and it is the cheapest fix in this document. Today `build_project` fails
with `QUEUE_MISSING` if the folder does not exist (`build_project.c:304`), and
`QUEUE` is a hardcoded `#define` in two files (`build_project.c:16`,
`run_application.c:11`). Add an `ensure_queue()` that resolves the workspace,
creates `Worker01` and `buildjobs` one level at a time if absent (the
`create_folder` journaling rules — intent, `FSpDirCreate`, flush, verify
non-alias folder — apply unchanged), refuses if a file or alias occupies the
name, and is invoked from a File > Prepare Build Queue command and before
first publication. Move the path into `config.h`/Preferences. The guest can
create folders on the AFP volume today; that is how sessions and projects are
written. The ssh step exists only to serve the hand-started worker.

Implemented 2026-10-08 (issue #65): `ensure_queue()` runs before first
publication in `build_project_step`, journalling `queue_intent` and
`queue_created` around each missing level and refusing a file or alias
occupying either name. Host checks cover creation, occupied names and the
pre-created queue. The File menu command and a Preferences queue path remain
open.

## What would make the whole process much smoother

Ranked by payoff:

1. **Native executor + self-serve builds.** The app publishes its job and, after
   acquiring `worker-lock` atomically, claims and executes it itself in the same
   cooperative loop. No MacRelix, no Perl, no fresh-filename publishing ritual,
   no background starvation, no manual start — and jobs still land in the
   durable queue as evidence. If an external lock exists, poll as today.
2. **App-created, Preferences-configured queue** (above). Kills the ssh step
   and the hardcoded path.
3. **Queue status surface.** A small window or status pane: queue path,
   who is serving (external lock vs the app), pending/claimed/completed counts,
   last job and result, and a stale-claim warning. Idea 004 §3 already wanted
   process-based liveness instead of lock-as-evidence; the Process Manager scan
   for MacRelix/ToolServer folds straight in.
4. **Per-command progress and real statuses.** The DoScript replies give stage,
   status and diagnostics as each command returns. Keep writing the same log
   files, but the app can now show live progress and return a truthful status
   per stage instead of one shell aggregate.
5. **One executor subsystem, three uses.** The same Apple-events code delivers
   ToolServer control, polite `quit_application` for launched artifacts, and
   the open-document mode trigger (ideas 004 §3 and this document are the same
   work).
6. **Host-side queue inspection.** A stdlib `tools/queue-status.py` that reads
   the share over ssh (pending/claimed/results/stale claims) makes "is it
   stuck?" answerable without touching the guest.
7. **Keep the escape hatches.** `worker.pl`, the fixed template recipe and the
   MacRelix transcript workflow stay as the verified fallback until the native
   path passes the same guest fixtures; the app must refuse unknown recipe ops
   rather than guess.

## Original recommended sequence

The implementation review below refines and supersedes this initial ordering.

Each step is independently useful and independently verifiable; nothing later
is advertised before its guest evidence exists (PLAN.md's rule).

1. **Guest spike — ToolServer client.** A diagnostic app
   (`SherclawkToolServerCheck`) finds or launches ToolServer, sends one fixed
   `Rez` command async, and logs `stat`, `----` and `diag` to a share log.
   Compare with the recorded `tlsrvr` transcript. Confirm no UI freeze and, if
   possible, no focus theft.
2. **Native step executor.** Rebuild the fixed PowerPC template through
   structured steps only, with no MacRelix running: same inputs, same
   `success.txt`, same artifact bytes as `worker.pl` produced.
3. **Integrate with `build_project`.** Publish as today, then self-execute when
   no external lock exists, else poll. Model-facing envelope and
   `read_build_log` do not change.
4. **Queue ownership.** `ensure_queue()`, queue path from Preferences, File
   menu toggle, status window.
5. **Queue worker mode.** Process external jobs (native jobs natively; legacy
   script-only jobs left for an external MacRelix worker or reported as
   unsupported — decide explicitly).
6. **Live acceptance.** The README's two accepted builds, a compiler-error
   repair, and a fresh revision-bound rebuild, all with MacRelix quit.
7. **Optional:** marker document / droplet, `tools/queue-status.py`, process
   liveness reporting.

## Implementation review and revised increments — October 6, 2026

The architecture is viable and implementation can start now. The native queue
producer and artifact verifier are already present; worker ownership, native
execution, and asynchronous outcome handling are substantial new work. “Almost
for free” above describes reuse of File Manager operations, not a finished
worker. MPW/ToolServer and its installed SDK remain dependencies.

Build incrementally, with the ToolServer guest spike as the first decision gate:

| Increment | Acceptance criterion |
|---|---|
| 1. ToolServer diagnostic | Successful and deliberately failing Rez commands reply asynchronously; event processing stays responsive; absent/quit ToolServer and late replies have truthful outcomes. |
| 2. Fixed native build | Compile, link, append resources, verify both forks and Finder metadata, and launch the existing template with MacRelix quit. |
| 3. Integrated self-builds | Publish existing snapshots, acquire exclusive ownership, execute validated steps, and retain compatible logs/results and artifact authorization. Pass independent and starter error–repair–rebuild fixtures. |
| 4. General queue service | Bounded scanning, unsupported-job skipping, queue creation, status display, and Serve Build Queue toggle. |
| 5. Convenience features | Preferences, open-document launch, and host inspection after the core contract is stable. |

Increments 1–3 deliver self-builds without the MacRelix worker. Keep the verified
Perl path available during rollout. Queue creation is also independently useful
and can land early. Preferences and document-launch modes are not prerequisites.

Resolve these details before integrating execution:

- Acquire the existing `worker-lock` atomically before serving; absence followed
  by claim is a race. Initially switch executors under exclusive ownership,
  rather than running both concurrently. Do not steal stale locks automatically.
  Queue ownership serializes these workers, not arbitrary third-party ToolServer
  clients; diagnostics need an idle ToolServer.
- The main loop lacks high-level Apple-event dispatch and `hello.r` declares
  `notHighLevelEventAware`. Install handlers, dispatch `kHighLevelEvent`, correlate
  replies by return ID and sender, and dispose descriptors on every path.
  `kAENeverInteract` is the API flag; interaction and foreground switching are
  separate concerns. Verify actual ToolServer behavior in the guest.
- Give the executor a lifetime independent of a chat run. Stop ends observation
  and prevents subsequent commands; it does not prove an in-flight command was
  cancelled. Drain late replies without advancing an abandoned build. Timeout,
  lost replies, and ToolServer disappearance are uncertain outcomes. Never
  replay automatically, including the upstream out-of-memory retry policy.
- Generate shell recipes and native steps from one validated build representation.
  Revalidating the existing snapshot descriptor avoids a separate steps file:
  maximum-size builds already occupy all eight `JOB_INPUT_MAX` slots. Adding a
  file requires revisiting that bound and the publisher tests.
- Centralize the default queue path first. Configurable paths must bind retained
  build IDs and persisted artifact authority to the originating queue; changing
  a preference must not redirect old IDs into another queue.
- Serving inside Sherclawk does not guarantee background responsiveness. Measure
  background execution and delays during Toolbox window/menu tracking.
- Verify artifact structure, resources, launch behavior and result contracts.
  Require byte-identical output only after establishing toolchain determinism.

Increment 1 is a separate `SherclawkToolServerCheck` diagnostic, not a production
executor. Its guest evidence and remaining limitations will be recorded below
and in `README.md`. Increment 2 and 3 acceptance is recorded below; general queue service remains unimplemented.

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

## Verification, when built

- Host: ASan/UBSan checks for everything testable off-guest — recipe→steps
  generation, claim/skip decisions, record writing and parsing, path and
  collision handling — extending `tests/test_build_project.c`,
  `tests/test_jobs.c` and `tools/check.sh`. The Apple-event layer itself is
  guest-only.
- Guest: new fixed diagnostics in the established pattern, each reporting
  `RESULT failures=…` into a share log; then fork-aware publication and real
  runs. Per AGENTS.md, host success is not guest evidence.
- Faults: unknown/malformed DoScript replies, ToolServer absent, ToolServer
  quit mid-command, oversized diagnostics, Stop between commands, stale lock,
  claim races with `worker.pl`. Uncertain outcomes never replay a published
  job.

## Open questions

- Does the async `dosc` reply work cleanly through `WaitNextEvent` on this
  install (and does the answer arrive while the window is being dragged)?
- Resolved in increment 3: revalidate the existing snapshot descriptor and use
  one shared command generator; no additional input slot is needed.
- Resolved in increment 3: keep `started` unchanged and write `native-executor`.
- What is the upper bound for a single reply (`----` + `diag` are one memory
  descriptor each; MrC can be chatty)? Truncate with an explicit marker, or
  redirect where possible.
- Can ToolServer be launched without the focus hop, and how does it behave if
  the user quits it mid-build? (Unknown outcome, presumably.)
- Queue default: keep `Retro68:Worker01:buildjobs` for compatibility, or make
  the native queue a new folder so the Perl workflow and the native one never
  share state? Compatibility argues for one queue; isolation argues for two.
- Do we still want the droplet / marker document once the in-app toggle and
  drop-on-app work?

## Evidence

- `worker/worker.pl:7-80`; `worker/README.md:25-111,141-182`;
  `README.md:178-188,190-207,495-505,529-553`.
- `build_project.c:16,171-196,255-270,301-316`;
  `jobs.c:65-223`; `run_application.c:11`;
  `main.c:1055-1081`; `templates/ppc-toolbox/build-native.sh`.
- `docs/sherclawk-agent-harness-research.md:697-775` (MacRelix,
  `tlsrvr`/ToolServer mechanism, the deleted `execute` event trap, and the
  file-job recommendation), especially `:741-751`.
- Upstream ToolServer client protocol (reference only):
  `metamage_1/lamp/jTools/tlsrvr/RunToolServer.cc` — `'MPSX'` discovery,
  `kAEMiscStandards`/`kAEDoScript`, `keyDirectObject` typeChar script,
  `'stat'`/`'----'`/`'diag'` reply parameters, status conventions. No code to
  be copied; AGPL-3.0.
- Vendored interfaces: `Processes.h` (`AppParameters`,
  `launchAppParameters`); `AppleEvents.h:87-91`
  (`kAEOpenApplication`/`kAEOpenDocuments`/`kAEAnswer`);
  `AERegistry.h:172,209`; `AEDataModel.h:222-224`; `AEInteraction.h:68-76`;
  `Retro68/libretro/start.c:31-41`; `Retro68/ImportLibraries/libInterfaceLib.a`.
- `PLAN.md:110-120` (boundaries) and the build/run contract.
