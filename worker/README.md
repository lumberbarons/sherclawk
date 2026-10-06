# MacRelix file-job worker

`worker.pl` executes foreground jobs inside the OS 9 guest. It uses Perl
built-ins and `/bin/sh`, with no host executor or code copied from MacRelix.
Sherclawk's native producer and bounded poller implement this protocol,
with an event-driven Toolbox diagnostic. `build_project`, revision binding
and launch tools remain subsequent work.

| File | Purpose |
|---|---|
| `../jobs.c`, `../jobs.h` | Native snapshot producer, strict completion parser and bounded log pages |
| `../tools/job-check.c`, `../tests/test_jobs.c` | Cooperative guest diagnostic and native protocol fault model |
| `worker.pl` | Singleton polling loop, rename claims, output capture and terminal records |
| `build-template.sh` | Run a snapshot of the verified PowerPC template |
| `../tools/publish-worker-job.py` | Stdlib host diagnostic producer with complete-file publication |
| `../tests/test_worker.py` | Execution, publication, exclusivity and crash non-replay checks |
| `VERIFIED.md` | Actual OS 9 guest results and retained evidence |

## Protocol 1

The queue must already exist. Each ID is 1–24 lowercase ASCII letters,
digits, underscores or hyphens; `worker-lock` is reserved. Lowercase avoids
HFS case collisions. Reserve the job folder with exclusive creation and never
reuse it, including after failed staging. Inputs have HFS-compatible names.
Only the producer owns unpublished files; after publication it must leave
all job files unchanged.

```text
queue/
  worker-lock/             present while worker owns this queue
  STOP                     optional: stop before another claim
  build001/
    script                 LF shell text, at most 65,536 bytes
    main.c, app.r, ...      closed snapshot inputs
    ready                   exactly "protocol=1\n"; published last by rename
```

Write and close every input, then write and close `ready.tmp` and rename it
to `ready` in the same folder. The diagnostic producer also fsyncs its files.
An unpublished folder is ignored. One worker reserves `worker-lock` by mkdir;
a second worker refuses to start. The worker renames `ready` to `claimed`,
publishes `started` by temporary-file rename, and executes `script` in that
job's directory, with stdin from `/dev/null` and separate `stdout` and `stderr`.
The singleton lock serializes the marker rename; this is not a lock-free
multi-worker queue.

The child closes its foreground log descriptors before returning. The worker
then publishes `result` by temporary-file rename. Records are ASCII/LF:

```text
protocol=1
id=build001
outcome=succeeded
exit=0
signal=0
wait_status=0
```

`outcome` is `succeeded`, `failed`, `signaled` or `rejected`. Rejection means
the publication marker or script was invalid; its record omits process status.
`exit`, `signal` and `wait_status` describe the waited shell, not necessarily
an individual MPW command. For example, a child command's signal may become
a normal nonzero shell exit. A valid terminal record must match the expected
protocol and ID and have the appropriate complete fields. Ignore `.tmp`
records. Claim and started records remain after completion.

Missing `result` after a claim means **unknown outcome**, including after a
deadline, Stop, worker error or guest crash. A started record, lock or unchanged
log does not establish liveness. Never recreate `ready` or resubmit the same
mutation automatically. Conflicting output files stop the worker and retain
the claim/lock for inspection. A malformed queue is not repaired automatically.

This queue is for trusted local recipes; it is not a sandbox or a model-facing
generic shell tool. Scripts must not fork background jobs, read interactive
input or mutate the queue protocol. Logs are unbounded on disk;
native polling reads at most one 1 KiB page from each log per poll. Source revision binding and artifact
authorization belong to the future build/run tool integration.

Close/rename gives complete-file visibility, not a transactional guarantee
across AFP server/guest crashes. Neither the installed guest nor this worker
has verified fsync-based power-loss durability. Preserve journals and inspect
the job's actual inputs, claim, logs and artifacts before manual recovery.

## Run in the guest

Publish the LF `worker.pl` data fork to the AFP share under a fresh filename
if replacing a previously read script (AFP caching was observed in template
work). Do not restart netatalk. In MacRelix:

```sh
perl -w /Volumes/Retro68/Worker01/worker.pl /Volumes/Retro68/Worker01/jobs
```

Append `--once` to process one scan and exit. Normal mode polls every second;
it checks `STOP` before each claim. Creating `STOP` lets a running foreground
job finish and prevents subsequent claims. It does not cancel a build. Normal
exit removes the worker lock. An error or killed worker leaves the lock.
Remove a stale lock manually only after establishing that no old worker or
job child is still active; claimed jobs remain excluded after unlock.

The installed Perl has no `strict.pm` or `warnings.pm`, no `fork()`, and no
three-argument descriptor-duplication open modes. `perl -w` supplies built-in
warnings. Execution uses `system()` with a literal shell command; no ID or
input path is interpolated into shell syntax. MacRelix's cooperative execution
lets other classic applications service events while the worker waits.

## Publish a native build snapshot

For a local host protocol check (the guest uses a queue on its mounted volume):

```bash
mkdir -p sherclawk/build/worker-jobs
python3 sherclawk/tools/materialize-native-template.py sherclawk/build/WorkerTemplate
python3 sherclawk/tools/publish-worker-job.py sherclawk/build/worker-jobs build001 sherclawk/worker/build-template.sh --input sherclawk/build/WorkerTemplate/main.c --input sherclawk/build/WorkerTemplate/app.r --input sherclawk/build/WorkerTemplate/build-native.sh
python3 sherclawk/tests/test_worker.py
```

For an actual native build, copy the producer and materialized sources to the
AFP server's temporary area, then invoke the producer as `macos9` with a queue
under `/srv/retro68`. Running it on the host-local example queue cannot compile
with MPW. Never copy a `ready` job incrementally into a live queue; publish the
marker at the destination only after all inputs have arrived.

`build-template.sh` runs the recipe under the fresh `build/native` folder.
Stage-start markers and returned ToolServer diagnostics appear in `stdout`
and `stderr`. ToolServer diagnostics arrive when each command returns;
an unchanged log during compilation is not evidence of a hang. Success
requires the matching `succeeded` result, the recipe's
`stage=complete status=0`, and `build/native/success.txt` naming `Template`.
Only then is `build/native/Template` a launch candidate. Do not launch partial
outputs from failed or uncertain jobs. Applications retain resource forks and
Finder info; transporting them requires the existing fork-aware workflow.


## Native producer and cooperative polling

`jobs_begin` accepts an already-resolved, trusted queue folder, a fresh lowercase
ID, immutable memory snapshots and a required flushed journal callback. It
refuses aliases at the queue leaf; the caller must resolve trusted ancestors
without following aliases. It reserves a new folder exclusively, records the
intent/reservation and never reuses a failed reservation. The caller
records the logical queue path alongside these records: native volume and
directory IDs are observational across remounts. This internal API
accepts trusted recipes only; it is not a model-facing shell tool.

Up to eight inputs use distinct lowercase ASCII HFS names, each at most 64 KiB,
with at most 128 KiB total. Exactly one is a nonempty `script`; its bytes contain neither
CR nor NUL. Worker protocol names are reserved. Source bytes are copied as
supplied, preserving MacRoman/CR inputs and LF shell recipes. The caller keeps
input buffers immutable through staging; revision binding is future work.

Call `jobs_step` once per event-loop turn. A staging step writes or verifies
at most 1 KiB, closes its data-fork descriptor, and returns. Closed inputs have
length/resource/alias checks and complete byte readback. Only after the staged
journal barrier does the producer write, close, flush and verify `ready.tmp`,
then rename it to `ready`. It records publication and never changes inputs
again. Failure at or after the rename attempt is conservatively unknown.
Failed staging leaves an unpublished, abandoned folder for manual inspection.

Waiting steps poll at most once per 60 ticks, reading at most a 256-byte result
and one 1 KiB page each from stdout and stderr. The terminal parser requires
exact protocol/ID, canonical LF fields, recognized outcome and consistent
bounded exit/signal/wait status. `.tmp` files are ignored; malformed or
unreadable results produce unknown outcomes. Logs have separate byte offsets;
missing logs produce empty pages, while truncation, aliases, resource forks
and read errors fail observation. Pages are raw bytes with explicit lengths,
so embedded NUL and partial encodings do not lose bytes. Consume each page
before the next step. `jobs_logs` supplies explicit bounded continuation after
completion; a terminal record does not mean all log bytes fit the first page.

Stop and elapsed-tick deadlines abandon staging or mark a published job
unknown. They never write queue STOP, kill a worker, delete a claim, recreate
ready or resubmit. Tick subtraction handles clock wrap. Journal failures stop
publication/observation. Terminal records establish shell completion only;
they do not authorize an artifact. No build or launch capability is advertised.
FlushVol does not establish verified power-loss durability across AFP.

The `SherclawkJobCheck` app uses this API from a `WaitNextEvent` loop and
retains a fresh job under `Retro68:Worker01:nativejobs:`. Create that queue on
the existing mounted share, then build/publish the diagnostic:

```bash
ssh beardmore 'sudo -n install -d -o macos9 -g macos9 -m 775 /srv/retro68/Worker01/nativejobs'
APP=SherclawkJobCheck sherclawk/tools/deploy-to-share.sh
```

Launch the diagnostic and run the existing worker against the fresh queue in
MacRelix (a short delay permits complete native publication):

```sh
cd /Volumes/Retro68/Worker01
open /Volumes/Retro68/SherclawkJobCheck
sleep 3
perl -w worker05.pl nativejobs --once
```

The diagnostic publishes a fixed LF script and 3 KiB CR snapshot, observes the result
and separate stdout/stderr, and writes `Retro68:SherclawkJobCheck.log`. It has a
120-second deadline; Command-period stops observation. Inspect the retained
folder after an unknown outcome. This verifies the native queue integration,
not model tool dispatch, source revisions, native compilation or launch.
