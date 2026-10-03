# MacRelix file-job worker

`worker.pl` executes foreground jobs inside the OS 9 guest. It uses Perl
built-ins and `/bin/sh`, with no host executor or code copied from MacRelix.
This implements the worker protocol; Sherclawk's native producer, polling UI,
`build_project` and launch tools remain subsequent work.

| File | Purpose |
|---|---|
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
input or mutate the queue protocol. Logs are currently unbounded on disk;
future native polling must read bounded pages. Source revisions and artifact
authorization belong to the future native producer.

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
