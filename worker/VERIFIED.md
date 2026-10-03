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

Native Sherclawk job production, revision binding, bounded result polling,
build/run tools, cancellation, recovery automation, log limits and power-loss
durability remain unimplemented or unverified. No new model tool is advertised.
