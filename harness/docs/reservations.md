# Exclusive guest reservations

`harness/manage.py` is the host manager for the OS 9 QEMU slots. It gives
each caller an exclusive lease before a guest is booted, keeps the QMP socket
private, and refuses every later operation when the token, owner, manager
epoch or deadline no longer matches. Agents in different worktrees can reserve
different slots; neither can type into, capture, renew or release
the other's guest.

One manager installs per host, outside every worktree. Source lives in this
repository under `harness/` (ADR-0009); state and evidence live in
`$OS9_HARNESS_HOME` (default `~/.local/share/os9-harness`), never in a
checkout or on the share. See [`provisioning.md`](provisioning.md) for the
immutable images and [`contract.md`](contract.md) for the native controller.

## Prerequisites

- Every slot provisioned and configured with the native controller, with
  `base/os9-mpw.qcow2` and `base/slotN.qcow2` present and read-only.
- QEMU on `PATH` and `qemu-img` available.
- The `Retro68` export **mounted on the host and writable**, because the
  manager reads fresh readiness from `Harness/<slot>/ready` and removes a
  stale `ready.tmp` left by an interrupted boot (issue #256). A read-only
  mount still reads readiness but cannot recover that staging file, so the
  slot is quarantined instead of booting without a heartbeat.

  ```bash
  export OS9_HARNESS_SHARE="$HOME/Retro68Share"
  ```

  Mount it with the host's normal AFP workflow; the manager never starts or
  restarts AFP and never changes the export.

## Command sequence

```bash
export OS9_HARNESS_SHARE="$HOME/Retro68Share"
python3 harness/manage.py status

token=$(python3 harness/manage.py --json reserve --owner agent-a \
        | python3 -c 'import json,sys; print(json.load(sys.stdin)["token"])')
python3 harness/manage.py screenshot "$token" --owner agent-a --out before.png
python3 harness/manage.py input "$token" --owner agent-a --text "Hello" --click
python3 harness/manage.py renew "$token" --owner agent-a
python3 harness/manage.py release "$token" --owner agent-a
```

`--json` gives structured output for the management commands (`reserve`,
`renew`, `screenshot`, `input`, `release`, `status`, `recover`, `suite`);
`run` passes the child's output through and returns its status. Lease commands
print the slot, generation, owner and remaining seconds; `reserve` and `renew`
also print the token. The token is the whole authority for that lease, so keep
it out of shared logs if other users can read them. `status` never prints
tokens.

Exit codes are `0` success, `2` for a stale/expired lease, no capacity or bad
usage (including a malformed `--step`), and `3` for a guest, host or recovery
failure. `suite` exits `1` when a step fails. `run` returns the child's own
status, so `2` and `3` are ambiguous there; a manager error is the one that
prints a `manage:` line on stderr.

### `run` and `suite`

`run` executes one host command under the lease. It re-reads the lease every
second and renews it with the lease's own TTL at least every 60 seconds (more
often for a short TTL, so a short lease neither lapses mid-command nor grows
to the default). It passes the lease to the child through
`OS9_HARNESS_TOKEN`, `OS9_HARNESS_OWNER`, `OS9_HARNESS_SLOT`,
`OS9_HARNESS_HOME` and `OS9_HARNESS_SHARE`; the child drives the guest through
`manage.py`, never through the private QMP socket. Commands are bounded by
`--timeout` (1..3600 seconds), and a lost lease or a timeout terminates the
child rather than leaving it running against a recovery; a revoked or expired
lease is noticed within about a second:

```bash
python3 harness/manage.py run "$token" --owner agent-a --timeout 120 -- \
    python3 harness/mailbox.py request ...
```

`suite` runs named commands in sequence under one lease, renewing between
steps and stopping at the first failure unless `--keep-going` is given. A step
that outlives `--timeout` fails that step (return code 124, reported as
`timeout`); a lost lease still aborts the suite:

```bash
python3 harness/manage.py suite "$token" --owner agent-a \
    --step "build::python3 harness/native/build.py" \
    --step "publish::tools/deploy-to-share.sh"
```

`COMMAND` is shell-split, not passed to a shell. `suite` is the lease-scoped
command runner; the registered native diagnostic suite (`suite.py`) layers its
own registry, per-check publication and evidence on top, and per-run
publication and evidence are `publish.py`
([publication.md](publication.md), [suite.md](suite.md)).

## Lease rules

- The slots are `slot1` and `slot2`. Their identities (MAC, QMP socket, overlay,
  pid file, log) are fixed by `provision.py` and are never shared.
- Every reservation gets a fresh random 128-bit token and an incremented
  generation. Ownership checks require the token, the owner string and the
  current manager epoch; the generation is bound to the token and recorded for
  audit and evidence.
- Default lease is **600 seconds**. `renew` extends it from the current time
  by at most 600 seconds. Idle reservations do not renew themselves; `run` and
  `suite` renew every minute while they are active.
- The wall-clock deadline is the expiry authority across processes. Within a
  matching monotonic clock source, expiry uses the earlier of the monotonic
  and wall-clock deadlines. Deadlines from another source (or from older
  manager databases without a source) are checked against wall time only;
  renewal records fresh deadlines and the renewing clock's source.
  Host sleep advances the wall deadline (macOS's monotonic clock may not), so
  a slept-through lease expires. On expiry the lease is revoked before the
  slot can be recovered or reassigned.
- `release` is idempotent: releasing a token again returns
  `"already_released": true` and does not touch a newer owner.
- A stale token can never renew, run, type, capture or release. Stale releases
  and stale post-expiry operations are refused or reported as no-ops; they do
  not stop the new owner's QEMU.

## Release, expiry and recovery

Release and expiry follow the same path, in this order:

1. Revoke the lease in SQLite (token and owner cleared; the token is recorded
   in the release audit) **before** any host or guest action.
2. Collect bounded evidence under `$OS9_HARNESS_HOME/evidence/<slot>/`: lease
   identity, host boot/epoch/timestamps, QMP and overlay paths, a bounded QEMU
   log tail and, when the guest still answers within three seconds, a PPM
   screenshot. Evidence failures are recorded; they never block termination.
3. Read the pid from the lease, the state row and the slot pid file. If they
   disagree, or the process command line is not this slot's overlay under
   `qemu-system-ppc`, the slot is quarantined and the process is **not**
   signalled. The manager never kills a process it cannot identify. If the
   recorded pid has since been reused by an unrelated process, an explicit
   `recover --slot` discards the stale pid (still without signalling it) once
   nothing answers on the slot's QMP socket and then resets the slot.
4. Send `SIGTERM`, wait for the exact pid to disappear, and quarantine if it
   does not. There is no `SIGKILL` fallback for a guest.
5. Delete the writable overlay and create a fresh one on the recorded base.
   A failed reset quarantines the slot.
6. Mark the slot free. The next reservation clears a stale controller
   `ready.tmp` left by the stopped boot, boots the slot and waits for a
   **different boot ID and an advancing heartbeat**; a stale `ready` file is
   not readiness. An unwritable staging file quarantines the slot instead of
   booting it blind (issue #256).

`reserve` also recovers expired and already-revoked rows before choosing a
slot, so an expired owner cannot indefinitely occupy capacity. Recovery is
serialised per slot with an OS lock; reserve itself is serialised across
processes with the global manager lock and SQLite transactions.

A quarantined slot is skipped by `reserve` and keeps its reason in `status`.
A boot that fails (no fresh readiness, an early exit, an unwritable staging
file) quarantines its slot even when cleanup succeeds, unless the lease was
already revoked and recovered by someone else, in which case the slot belongs
to that recovery. Clear a quarantine only after fixing the cause:

```bash
python3 harness/manage.py status --json
python3 harness/manage.py recover --slot slot1 --reason "overlay repaired"
```

`recover` without `--slot` simulates a manager restart. It rotates the manager
epoch, invalidates every lease and recovers every non-quarantined slot.

## Manager restart and host restart

The manager records the host boot identity (`kern.bootsessionuuid` on macOS,
which a clock step cannot change, falling back to `kern.boottime`;
`/proc/sys/kernel/random/boot_id` on Linux) and the last monotonic time with
its source. `Clock` reads `time.clock_gettime(time.CLOCK_UPTIME_RAW)` when
available, otherwise `CLOCK_MONOTONIC`. These OS clocks have a shared origin
across host processes, including processes using different Python interpreters.
This matters on macOS with `/usr/bin/python3` 3.9.6, where
`time.monotonic()` can start near zero in each new process; comparing those
readings would mistake an ordinary CLI invocation for a restart and revoke
live leases. If neither OS clock is available, the manager falls back to
`time.monotonic()` with an identity unique to that Clock instance, so its
persisted readings are never compared with another instance's readings.

The database records the source alongside both `last_mono` and each lease's
`expires_mono`. A source change, including an upgrade from a database without
source tracking, preserves the epoch and leases and uses wall time for those
leases. A new host boot identity, or a monotonic clock that moved backwards
within the same source, means the host or manager restarted: every previous
lease is invalidated, tracked instances are stopped and reset, and failures
are quarantined. Recovery resets the saved monotonic
baseline so subsequent commands do not repeat the restart. A clean shutdown
of the manager itself is not a restart — the CLI is stateless and the host
identity persists — so leases survive between commands.

Host sleep is not a restart. Leases lapse through the wall deadline instead.

## What this is not

This is cooperative coordination **between agents running as the same host
user**. The OS lock, SQLite state and random token stop accidents and stale
aliases, not a determined same-user process: that account can read the state
file and signal the QEMU processes directly. Treat the token as an accident
boundary, not a secret from the account that owns the harness.

## Acceptance

Host suites (no QEMU, VM or network) run with the rest of the harness tests:

```bash
python3 -m unittest discover -s harness/tests
```

The opt-in guest acceptance boots every slot, releases and reassigns one,
proves stale-token refusal, verified termination and readiness gating, and
deliberately fails an overlay reset to demonstrate quarantine and recovery:

```bash
OS9_HARNESS_SHARE="$HOME/Retro68Share" \
    python3 harness/tests/guest_reservations.py
```

Run it only when no agent holds a slot: the initial clean step invalidates
leases and recovers every slot.

Evidence from a passing run is recorded in
[`verification.md`](verification.md). It never restarts AFP and never
force-quits an unverified process.
