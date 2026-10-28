# Publishing diagnostic runs

`harness/publish.py` publishes one built application into a fresh private run
workspace on the leased slot's own `Retro68:` volume, publishes the mailbox request
last, and retains evidence that identifies the exact source, artifact and run.
It is lease-aware: every publishing step re-checks the reservation, so an
expired or reassigned lease cannot publish, and the share host is the only
place the netatalk sidecar and Metadata xattr can be written.

Sherclawk supplies the built artifact and the registered target name; the
harness owns staging, fork metadata, publication and collection
(ADR-0009). Sherclawk's `tools/deploy-to-share.sh` is a consumer of the same
transport for the shipping application.

## Configuration

```bash
python3 harness/provision.py storage show          # each slot's mount and export
export OS9_HARNESS_SHARE_HOST="<afp-host>"         # ssh target that carries the exports
```

Each slot has its own mounted volume and server export, recorded by
`provision.py storage set` ([provisioning.md](provisioning.md#per-slot-afp-storage)).
A run uses only the leased slot's: the host mount for the mailbox and the
diagnostic log, the server export for staging and installation. Before the run
is created, before the application is published and again before the mailbox
request, the publisher requires the slot's mount and its export to both carry
that slot's `.harness-slot` sentinel, so a mapping that points at another
slot's volume or the developer share is refused while nothing has been written.
`OS9_HARNESS_SHARE_DIR` only names the export `install` is given when it is
used outside a lease.

The share host needs passwordless `sudo -n`, a `macos9` user and group and
python3, exactly like `deploy-to-share.sh`. The publisher never restarts AFP
and never changes the export; it only creates new, never-reused paths and
retains them.

## Run layout

A reservation names its runs by slot and generation; a run never reuses a
directory:

```text
Retro68:Harness:<slot>-g<generation>:<run>:
    <Diagnostic>            the selected application (data fork)
    ._<Diagnostic>          its netatalk resource-fork sidecar
    buildjobs:              the run's private native build queue
    <fixture files>         the caller's fixture inputs
    <Diagnostic>.log        the diagnostic's own guest log
Retro68:Harness:slot1:      the controller mailbox (request/claim/result/stop)
```

The paths are relative to the slot's volume, so the same names exist in each
slot's volume without sharing a file. Two reservations therefore share nothing:
distinct volumes, workspaces, applications,
sidecars, queues, fixture files, mailbox files, logs and evidence. The
publisher refuses to publish when the mailbox still holds an unconsumed
request, so an old in-flight run can never be replayed by accident.

## Build before reserving

```bash
python3 harness/publish.py verify --build build --target SherclawkWriteCheck
```

`verify` derives the data fork (`build/<target>.APPL`) and the Rez resource
fork (`build/.rsrc/<target>.APPL`) from the selected target, rejects
unregistered targets and missing or empty forks, and prints both SHA-256
hashes. Build every selected diagnostic before reserving a guest.

## Publish and collect

```bash
token=$(python3 harness/manage.py --json reserve --owner agent-a \
        | python3 -c 'import json,sys;print(json.load(sys.stdin)["token"])')
python3 harness/publish.py run \
    --build build --target SherclawkWriteCheck \
    --token "$token" --owner agent-a \
    --fixture scenario=scenario.txt --json
```

`run` publishes in this order, and stops at the first failure:

0. Validates the seconds and every fixture (name, size, duplicates, names the
   workspace reserves) before touching the share host.
1. Creates the run workspace and its `buildjobs:` queue, naming every
   directory level for the share owner.
2. Stages the data fork and sidecar under a never-reused `/tmp` scratch
   directory on the share host, installs both into the workspace and writes
   the complete netatalk Metadata xattr.
3. Verifies server-side that the data fork and resource fork match the built
   hashes and that the Finder type/creator record is complete and current.
4. Publishes each `--fixture NAME=LOCALFILE` as a bounded file in the
   workspace (a single 1..31 byte HFS name, at most 1 MiB).
5. Requires an advancing controller heartbeat for the slot (a static `ready`
   file is not liveness), then publishes the closed `HARNESS/1` request into
   the slot mailbox last.

Then it waits for the controller's closed `result-<run>` (bounded by
`--seconds + 15`, override with `--wait`), captures a final screenshot under
the lease, copies the QEMU log tail, the controller trace and the guest
diagnostic log, and writes the run evidence. Retention also runs when the wait
times out or the result is invalid, since those are the runs that need it.
The result is keyed by the run, so it is read even after the lease lapsed, but
the screenshot and QEMU log belong to the slot (possibly another owner's by
then) and are skipped, with a note in `captures`, unless the lease is still
held. Exit codes are `0` passed, `1` a
closed non-pass, `2` stale lease or usage, `3` publication or host failure.
`--no-screenshot` skips the capture.

A failed or missing result is never a pass: a timeout reports `unknown`
(also the evidence `phase`), malformed evidence is refused, and nothing is
replayed. Every failure after the run began names its evidence directory on
stderr. The unattended suite runner (`suite.py`, [suite.md](suite.md)) calls
the same publication path for every registered check and never retries one.

## Evidence

Every run writes `evidence.json` under
`$OS9_HARNESS_HOME/evidence/<slot>/<slot>-g<gen>-run-<run>/` at each state
change, so a crashed or failed run still identifies itself, and repeated
launches accumulate side by side with nothing deleted:

- source revision and dirty state of the checkout containing the build
  directory, as of publication. Git cannot say which commit produced a binary,
  so `artifact_predates_head` and `artifact_predates_changes` flag a build older
  than the commit or the uncommitted changes; treat either as a possible
  mismatch
- both artifact fork hashes, sizes and local paths
- slot, generation (decimal and 32-hex), owner, guest boot ID and boot clock
- manager epoch and host boot identity
- emulator version and the exact QEMU argv for the slot
- the request (deadline and the saved `request.bin`), fixtures and their
  hashes, workspace/application/queue paths
- the closed result with named checks, timings and per-step detail
- screenshot, QEMU log tail, controller trace and diagnostic log

## One application without a run

```bash
python3 harness/publish.py install --build build --target Sherclawk \
    --dest /srv/retro68/Sherclawk
```

`install` is the lease-free transport `tools/deploy-to-share.sh` uses for the
shipping application: stage, install both forks, write the xattr, verify, and
fail closed on mismatch. It accepts any built application name, not only
registered diagnostics, but the destination must be named for the target.
Files are installed before they are verified, so a failed verification leaves
the destination unverified, possibly over an earlier good copy; do not launch
it until a publication succeeds. The destination's parent directory must already
exist; `install` creates no directories (a run creates its own workspace).

## Host tests

```bash
python3 -m unittest discover -s harness/tests
```

The publish suites run a fake ssh/scp boundary, a fake reservation manager and
a local share directory: concurrency isolation, failure at every staging
boundary, target mismatch, lease expiry, boot change, leftover queue refusal
and retained evidence are all covered without QEMU, a VM or a share host.
