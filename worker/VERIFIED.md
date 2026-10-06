# Worker verification — October 3, 2026

Tested with the OS 9.2.2 / MacRelix / MPW installation identified in
[`../templates/ppc-toolbox/VERIFIED.md`](../templates/ppc-toolbox/VERIFIED.md).
The executor ran in MacRelix; the host only published inputs, inspected files,
and sent QMP input. No AFP restart, VM flag change or cross-compilation was used.

Final worker SHA-256, matched against guest `Worker01:worker05.pl`:

```text
59e9898dc39043d8ecb5e0102ff353544fbf58772f5b1b277b8adc51817b5270
```

`build-template.sh` SHA-256:

```text
bd56fd84a727a736d536773f516c7e1f038b943a8757031c899f6f2914f3a88c
```

Guest evidence remains on the AFP volume under `Retro68:Worker01:`:

| Fixture | Observed result |
|---|---|
| `jobs:smoke` | Exit 0, separate expected stdout/stderr; execution-count stayed at one after a second worker scan |
| `jobs:failure` | Exit 7, `failed`, diagnostic retained in stderr |
| `jobs:buildbad` | Exit 1; MrC filename/line error in stderr, no executable or success record |
| `jobs:buildgood` | Compile/link/Rez completed; exit 0 and `artifact=Template` success record |
| `jobs:staged` | No ready marker; left unclaimed and unexecuted |
| `jobs:pollone` | Completed during continuous polling |
| `jobs:signal` | `signaled`, signal 15, raw wait status 15 |
| `jobs:pollsleep` | Published after polling started; completed after STOP arrived during its sleep |
| `jobs:polltwo`, `jobs:zzblocked` | Retained ready markers and no claims after STOP; normal worker exit removed its lock |
| `finaljobs:nativefinal` | Final worker and shipped build wrapper returned exit 0, complete stage and successful artifact record |
| `finaljobs:invalidcr`, `finaljobs:badmarker` | CR script and protocol-2 marker rejected without execution; subsequent scan did not replay them |

`worker04.status`, `replay04.status`, `polling05.status` and `final05.status`
were all 0, as were `reject05.status` and `noreplay05.status`.
ToolServer output was captured in the per-job files.
MacRelix `open finaljobs/nativefinal/build/native/Template` launched the app;
a guest screenshot showed `Sherclawk native template` and both expected text
lines. Command-Q quit it. This is visual launch verification, not an automated
runtime test protocol. Screenshot evidence is local under ignored build output.

Initial attempts established installed-Perl limitations: module loading of
`strict.pm` failed, `fork()` was unimplemented, and the three-argument `<&`
open mode was unsupported. Their job trees and locks were preserved by
same-volume rename into `fork-attempt` and `dup-attempt`; they were not replayed.
The final worker uses only built-ins and `system()` with shell redirection.

Seven stdlib host checks passed via `python3 sherclawk/tests/test_worker.py`:
success/failure/no replay, incomplete/rejected jobs, Stop/conflicting outputs,
signal status, live publication/graceful Stop, singleton/crash non-replay,
and producer validation. The crash check kills the host worker after execution
starts, verifies the retained claim/lock and absent result, removes the lock
explicitly after the child finishes, and verifies no replay. Intentional worker
crash and output conflicts were tested on the host, not against the live guest.
`perl -w -c` and `git diff --check` passed.

At the worker-only milestone, native production/polling and build/run tools
remained future work. The next section records native producer/poller
verification; no new model tool is advertised.


## Native producer/poller — October 3, 2026

`SherclawkJobCheck` was cross-compiled with Retro68, published fork-aware,
and launched in the same OS 9.2.2 guest. Its own File Manager calls created
`Retro68:Worker01:nativejobs:` jobs; the host did not publish ready or the
snapshot inputs. The existing `worker05.pl` executed the published script in
MacRelix. The diagnostic serviced `WaitNextEvent` between bounded staging and
polling steps.

| Evidence under `Retro68:Worker01:` | Observed result |
|---|---|
| `nativejobs:native-0003564b`, `nativecheck01-producer.log` | Initial fixture exposed absent shell `printf`; shell returned success because of a trailing `exit 0`, but diagnostic correctly reported failure for missing expected logs |
| `nativejobs:native-00045e3c`, `nativecheck02-producer.log` | Corrected fixture used `cat`/`echo` with explicit failure propagation; native 3 KiB snapshot staging/readback and multiple log pages completed with `RESULT failures=0`, stdout 3,095 bytes, stderr 23 bytes |
| `nativejobs:native-00046c21`, `nativecheck03-stopped.log` | Command-period during waiting recorded `job_stopped`, state unknown; ready and both inputs remained, with no claim/result or recreated publication at Stop; a later explicit worker scan consumed that original ready marker and completed it |
| `nativejobs:native-0004a7ab`, `nativecheck04-producer.log` | Final rebuilt/published diagnostic returned `RESULT failures=0`, stdout 3,095 bytes and stderr 23 bytes |

The successful job's script retained LF bytes, its 3,072-byte snapshot ended
in `78 0d` (x/CR), and its result matched protocol 1, its exact ID, succeeded,
exit 0, signal 0 and wait status 0. The worker's `nativecheck.status` was 0.
The diagnostic intentionally reports a non-success result when stopped.
No worker was left running after these checks. The stopped observer
retained its unknown result; the later explicit worker scan completed the
original queued job without recreation or resubmission. This confirms that
Stop ends observation and does not cancel execution.

ASan/UBSan native File Manager model checks passed for exclusive reservation,
reserved/duplicate names, script CR refusal, byte-exact multi-page and 64 KiB
staging, zero-length non-script inputs, short writes, corrupt readback, close
and flush failures, ready verification, rename uncertainty, journal failures
at each barrier and after terminal observation, strict result parsing and
numeric overflow refusal, truncated/mismatched/malformed records, alias and
resource refusal, `.tmp` ignoring, separate bounded log pages and continuation,
log truncation, polling rate, Stop, deadlines and tick wrap. The existing
seven Python worker checks also passed. Fault injection was on the host model,
not on the live guest volume.

This verifies native snapshot publication and cooperative polling through a
diagnostic app. Model-facing dispatch, source revision binding, native compiler
job submission, artifact authorization, cancellation, automatic recovery and
AFP power-loss durability are still outside this slice.

## Model-facing native builds — October 5, 2026

`SherclawkBuildCheck` used native ordinary file tools to assemble
`Retro68:BuildCheck00025ac3:` without `create_project`: two C sources, one Rez
resource, a declared header, include path and an editable protocol-2 descriptor.
It then used `create_project` for `BuildCheck00025ac3s`, added a second C source
and edited the descriptor's source list/output name. The native build producer
published all snapshots; the host did not create ready markers or input copies.

| Build ID in `Worker01:buildjobs:` | Observed result |
|---|---|
| `build-00025b49-0001` | Independent project: deliberate `#error`, exit 1, no authorized artifact |
| `build-00025ce6-0002` | Guarded source repair, fresh revision snapshot, exit 0, artifact `build:native:independent` |
| `build-00025f5f-0003` | Starter with added source/output `starter`: deliberate `#error`, exit 1 |
| `build-000260ff-0004` | Guarded repair, fresh snapshot, exit 0, artifact `build:native:starter` |

The diagnostic reported `RESULT failures=0`. Both exact successful artifacts
were manually launched with MacRelix `open`. The independent app displayed
`Two source files compiled natively`; the starter displayed its native template
window. Both quit through Command-Q. This validates compilation and visual
launch, not automated runtime correlation or the future `run_application` tool.
The worker exited normally after STOP; its original Stop marker was retained
by rename before subsequent explicit verification. No AFP restart was needed.

The first independent fixture lacked a `QDGlobals` definition. Both its
deliberate compiler error and later linker error were correctly returned as
failed builds, with no artifact path; those original jobs remain retained as
`build-000215cf-0001` and `build-000217d9-0002`. After correcting the fixture,
the four-build acceptance above passed. Diagnostic faults were injected only
on the host File Manager model.

A live main-app check exposed colon-terminated folder paths from `list_files`;
`build_project` now accepts those directly, with a host regression check. Stop
was exercised while the model's `build-0002ac3e-0001` was published: the app
recorded an uncertain outcome with no artifact, retained its original ready
marker and stopped further calls. After the worker was explicitly resumed, it
consumed that same original job and returned succeeded/exit 0. The earlier
observer's uncertain result remained unchanged; no ready marker or snapshot was
recreated. The retained session is `Sherclawk Sessions:s0002a455.jsonl` (local
ignored copy `build/build-first-session.jsonl`). The idle MacRelix worker required
foreground activation to notice STOP in this live check; presence of a worker
lock never establishes that background polling is progressing.

The final main app's `openai/gpt-6-luna` loop built the independent fixture as
`build-0002ed03-0001`, received succeeded/exit 0 and its exact artifact path,
then called `read_build_log` at stdout offsets 0 and 128. The two pages covered
all 151 bytes through `stage=complete status=0`. All six calls/results paired
in `Sherclawk Sessions:s0002e7bf.jsonl`, and the model returned a final successful
build answer naming that ID. The completed session is retained locally under
ignored `build/build-session.jsonl`. The idle worker was explicitly activated
in MacRelix before claiming this snapshot; no host executor compiled it.
