# Native controller and diagnostic contract, version 1

`native/` is the standalone PowerPC Startup Items controller, its host-testable
cooperative state machine, closed-file I/O and public diagnostic adapter. It
includes no Sherclawk headers or helpers. The Sherclawk consumers are
`tools/write-check.c` and `tools/inspect-check.c`; their compiled default
workspace and manual logging remain available when no harness config exists.
No API key or running Sherclawk agent is needed.

## Build and install

Build before booting or reserving a guest:

```bash
./build.sh HarnessController_APPL
./build.sh HarnessInstall1_APPL
./build.sh HarnessInstall2_APPL
./build.sh HarnessAcceptance_APPL
./build.sh SherclawkWriteCheck_APPL
./build.sh SherclawkInspectCheck_APPL
```

Publish those applications fork-aware to the existing `Retro68:` export. Use
`tools/deploy-to-share.sh` with explicit `SHARE_HOST` and `APP` for Sherclawk
consumers; the controller and helpers use the same MacBinary or
APPL/resource-fork/Finder-metadata transport. Never restart AFP.
Create `Retro68:Harness:slot1:` and `Retro68:Harness:slot2:` on the export.
Each mailbox has one writer; exclusive ownership is the reservation manager's
job (#71), not a protection provided by these files.

Boot the writable slot overlays. Launch `HarnessInstall1` once in slot1 and
`HarnessInstall2` once in slot2. Each installer copies both controller forks
into the **boot disk's** Startup Items folder and publishes a closed
`Harness slot` Preferences file (`slot1` or `slot2`, followed by CR). It refuses
an existing destination. Inspect `Retro68:HarnessInstallN.log` for
`RESULT installed=1 slot=N`, then shut the guest down cleanly. Seal each
configured overlay and create disposable runtime overlays:

```bash
python3 harness/provision.py freeze-slot slot1
python3 harness/provision.py freeze-slot slot2
python3 harness/provision.py init slot1
python3 harness/provision.py init slot2
```

The resulting read-only `base/slotN.qcow2` retains its shared immutable
`os9-mpw.qcow2` backing file and private slot configuration. Checksums are in
`manifest.json`; both the shared base and slot base are checked before launch.
`reset` removes only the runtime overlay. Keep all backing files at their
recorded paths. Archive and rebuild configured slot bases when updating the
controller; never replace an image under an overlay or freeze a forced-off
or uncertain guest.

At boot the controller allows up to 180 guest seconds for AFP to auto-mount.
It publishes `ready` only after reading the private slot identity and verifying
there is no leftover `Harness active` configuration. Readiness is a closed,
replaceable heartbeat, updated once per guest second:

```text
READY/1<CR>
slot1<CR>
<32 lowercase hex boot ID><CR>
<guest clock seconds since 1904><CR>
END<CR>
```

The boot ID combines guest time, high/low microseconds and controller PSN.
Record the old boot ID before recovery and require a different ID afterwards
(`mailbox.py ready --previous-boot`). Also observe heartbeat advancement; file
existence alone does not prove readiness. A quarantined controller removes its
readiness. Deadlines use this **guest clock**, avoiding host/guest timezone or
wall-clock differences. They must be within 600 seconds when claimed.

## Request and report framing

The public codecs are `contract.py` and `native/contract.h`. Wire files use
printable ASCII and CR record separators, including a final CR. No escaping,
blank extension fields or trailing bytes are accepted. HFS paths are absolute,
contain no empty/parent component, are at most 220 bytes and have components
of at most 31 bytes. Workspace paths end in a colon. This v1 protocol restricts
paths to ASCII even though the diagnostic's file tools support MacRoman.

A request contains these ordered records:

```text
HARNESS/1
slot1 | slot2
reservation generation (32 lowercase hex)
boot ID (32 lowercase hex)
run ID (32 lowercase hex)
registered diagnostic name
exact application HFS path
private runtime workspace HFS path
absolute deadline on the guest clock (unsigned 32-bit decimal)
END
```

These examples show records on separate lines for readability; the actual
separator is CR. Requests are smaller than 1024 bytes. Registered names are
`SherclawkWriteCheck`, `SherclawkInspectCheck`, and the explicit test fixture
`HarnessAcceptance`. The application's final path component must equal its
registered name. Create the workspace and publish all fixture inputs before
publishing the request. The caller grants the exact application and workspace
paths; this is a same-user coordination protocol, not a security boundary.

Write and close a staging file, then publish it atomically as mailbox
`request`. The controller checks the slot/boot, renames it create-only to
`claim-<first 20 run hex digits>`, flushes and rereads that claim before doing
anything else. A claim collision prevents launch, including across controller
restarts. Prefix collisions conservatively refuse work; the complete identity
is always in the body. Malformed/wrong-boot requests remain unclaimed evidence
and produce no result; the caller must treat them as rejection/unknown, never
success. Do not overwrite or delete evidence to trigger replay.

The controller writes, flushes, closes, reads back and renames the complete
request to `Harness active` in boot-disk Preferences **before** launch. It
rejects aliases and an already running instance of the application. A launch
error is uncertain (`unknown`), never automatically retried. It observes only
the returned PSN and yields between each controller phase.

The diagnostic calls `harness_begin(name)`, verifies the requested name and
its own exact Process Manager application spec, then uses
`harness_workspace()` as its runtime root. Missing private config means manual
mode; unreadable or malformed config means exit with error, without falling
back to the manual workspace. Each `harness_check(condition, name)` retains
a named result and yields/checks Stop and deadline at a bounded assertion
boundary. `harness_finish(failures)` writes a closed terminal report and the
application returns from main automatically. Executor failures outside named
checks become an explicit failure assertion. Early returns without finish
cannot pass.

A report repeats the first nine request records, replaces request `END` with
an outcome, and appends:

```text
passed | failed | harness_error | stopped | unknown
1 assertion name (or 0 for failure; one record per assertion)
TOTAL <assertion count> <failure count>
END
```

At most 256 assertions, each name 1..120 printable ASCII bytes; report size is
less than 32768 bytes. Repeated names are permitted for paged assertions.
Counts are recomputed, not trusted. `passed` requires at least one assertion
and no failures; `failed` requires at least one failed assertion. Every request
field must match, including generation, boot, run, diagnostic, paths and
deadline. Unsupported versions, partial/stale/conflicting/malformed reports
cannot pass.

The diagnostic publishes `report-<run prefix>` by closed create-only
staging/rename. The controller independently validates it and, after observing
the process exit for ordinary passed/failed reports, publishes
`result-<run prefix>` with the same body. Host consumers accept **result**, not
the diagnostic's intermediate report. A closed malformed report gives
`harness_error`; disappearance without a report and deadline give `unknown`.
A matching closed request at `stop-<run prefix>` gives `stopped`; a pre-launch
Stop prevents launch. Stop is an observation outcome, not proof of cancellation
or rollback. No process is force-quit.

Only passed/failed plus observed exit clear private configuration and admit a
new request. Unknown, stopped, harness errors, uncertain claim durability,
publication failure or cleanup failure quarantine the controller until guest
recovery. Preserve the mailbox, logs and run fixtures before resetting the
runtime overlay. A leftover private active config prevents fresh readiness on
controller restart. Do not remove it to make a reused uncertain guest healthy.

## CLI and acceptance

On the export filesystem (locally, or mounted with create-only hard-link
publication support), `mailbox.py ready`, `request`, `result`
and `stop` implement a single exchange. `request --save` preserves exact local
request evidence before publishing; `result` returns zero only for a validated
pass, one for a validated nonpass, and two for missing/invalid evidence. The
CLI neither reserves guests nor retries launches.

Host tests run in `tools/check.sh`: C protocol and controller-state fault
suites plus stdlib Python codec, publication, readiness and provisioning tests.
`HarnessAcceptance` reads a closed `scenario` file in its private workspace:
`pass`, `fail`, `early`, `partial`, `identity`, `target`, `badconfig` or `wait`.
`target` deliberately mismatches the diagnostic name; `badconfig` corrupts the
private config then verifies rejection without a manual fallback. The opt-in guest runner
creates fresh private fixtures and asserts the expected outcome:

```bash
python3 harness/tests/guest_acceptance.py write --slot slot1 \
  --mailbox /mounted/Retro68/Harness/slot1 --runs /mounted/Retro68/Harness/runs \
  --guest-runs Retro68:Harness:runs:
```

Run `write`, `inspect`, `pass`, `fail`, `duplicate`, `early`, `partial`,
`identity`, `target`, `badconfig`, `deadline` and `stop`. The duplicate case leaves an unconsumed
request deliberately; retain it, then recover before the next case. After any
quarantining case, preserve evidence, reset/init/launch the slot, and require
fresh boot readiness before proceeding. Missing final results time out as
unknown and are never replayed. The runner requires an already-owned guest;
it does not implement #71's reservation or #72's remote publication/evidence
manager. Record actual guest results separately from host-test evidence.
