# Two-guest qualification — October 23, 2026

Verified the working tree on top of `c0eacc7` (issue #74, ADR-0009). QEMU
10.1.2 `mac99` slots booted from the immutable configured slot images, with
the `Retro68` export mounted on the host over NFS from the AFP host
(`mount_nfs -o vers=3,noresvport,nolocks,locallocks,noac,nonegnamecache`).
`tools/check.sh` (including the new two-agent, timing and skill suites) and
`tools/lint.sh` passed; `./build.sh HarnessChecks_APPL` built every
registered diagnostic in 1 minute 30 seconds. The regression suites in
`harness/tests/test_qualification.py` run two suites against one manager
with the second inside the first's wait for its result: a deliberate
assertion failure, a stalled guest, a released owner's token replayed
against a live suite, an expired lease whose slot is reassigned, and
repeated runs that never reuse a workspace or boot.

`python3 harness/tests/guest_reservations.py` returned `ACCEPTED two-slot
reservation manager` again, with the same isolation, stale-owner,
termination, quarantine and recovery results as the earlier record.

`python3 harness/tests/guest_qualification.py --build build` ran the
`write` and `inspect` checks plus a `deliberate-failure` check
(`HarnessAcceptance` told to fail, expected to fail) and returned `ACCEPTED
two-guest qualification`. While two agents' suites ran at once, the two held
distinct slots with distinct boot identities, a third reservation was refused,
and a made-up token was refused under either owner's name; every check in both
suites ran in its own never-reused workspace and each suite stayed on its own
slot. Both slots were free and stopped at the end.

| Phase (seconds) | One guest | Two guests, qualify-a | Two guests, qualify-b |
|---|---:|---:|---:|
| Boot to controller readiness | 47.2 | 50.0 | 41.6 |
| Publication | 12.8 | 15.1 | 12.9 |
| Diagnostic execution and result collection | 38.1 | 38.1 | 42.8 |
| Suite wall time | 99.4 | 104.3 | 98.4 |

One guest completed three checks in 99.4 s (about 109 checks per hour); two
guests together completed six in 104.3 s (about 207 checks per hour). Two
slots therefore roughly doubled throughput for this registry, with no sign of
contention between the guests, but the measurement is one pass on one host
over three short checks; the pool stays at two slots. After the runner was revised (it now also refuses a live token under the other
owner's name, cleans up on failure and drains both suites' output) it passed
again under `uv run python`: one guest completed three checks in 91.5 s (about
118 checks per hour) and two guests together six in 96.4 s (about 224 checks per
hour). Raw timings are in
`build/qualification/` (untracked); each suite's evidence is under
`$OS9_HARNESS_HOME/evidence/suites/`.

Problems the pass found, each kept as evidence and none counted as a pass:

- An earlier pair run failed its third check on slot2 with `no advancing fresh
  controller heartbeat` while the NFS mount used default attribute caching; the
  heartbeat is replaced by rename and the cached directory entry looked stale.
  The same pass succeeded with `noac,nonegnamecache`, which the mount above
  uses.
- The first full registered-suite run stopped at `search` (`unknown` after the
  195 s wait) and left its `request` in `Retro68:Harness:slot1:`. Recovery does
  not clear that file, so the next suites on slot1 failed at publication until
  it was moved aside by hand into `Harness/archive/` on the share (#260).
- `SherclawkSearchCheck` and `SherclawkEditCheck` then never ran, even alone on
  a fresh guest: the controller in the sealed slot images was built for #255
  and registers fewer diagnostics than `DIAGNOSTICS` now lists, and it ignores
  an unregistered request silently (#261). Both slot bases were rebuilt with the
  current controller: the previous bases were archived and later deleted,
  `HarnessInstall1` and `HarnessInstall2` each logged `installed=1`, each guest
  was shut down from Special > Shut Down, and `provision.py freeze-slot` and
  `init` recorded new checksums.
- A suite using the full registry then lost its lease at the self-build check
  (`lease_lost`, `unknown or stale lease token`). A `manage.py status` run in
  another process had recorded `manager_restart`: under the host's
  `/usr/bin/python3` each process reads its own `time.monotonic()` origin, and
  the manager compares those readings across processes (#262). The self-build
  checks were rerun with every harness process under `uv run python`, whose
  monotonic clock is the host uptime, and no other manager command ran (Resolved by #262:
  the manager now reads the OS uptime clock directly, so processes share one
  monotonic origin and the workaround is no longer needed.)

With the current controller in both slots, a full registered suite on one guest
passed `write`, `search`, `edit`, `inspect`, `project`, `scroll` and `handoff`
(wall 351 s to the interrupted self-build check; about 47 s boot, 27 s
publication, 270 s diagnostic execution and collection). A second suite then
ran `selfbuild` and `selfbuild-stop` to a pass: 25 and 6 assertions, 540 s and
83 s, with a fresh reservation (new boot, 44 s) between them and a wall time of
717 s. `selfbuild` used 540 of its 600 s timeout, so it has little headroom on
this host. Both builds ran through MPW ToolServer on the shared base, which
contains no MacRelix (see [provisioning](provisioning.md)); neither used an API
key or a Sherclawk agent session. The registry was not run end to end in one
uninterrupted suite.

Guest images, credentials and the raw evidence stay outside the repository.

# Unattended suite host verification — October 23, 2026

Verified the working tree on top of `14bed6d` (issue #73, ADR-0009).
`tools/check.sh` passed the ASan/UBSan C suites, the native controller suites,
the stdlib Python harness tests (including the new suite/registry, failure,
reset, session and interactive tests) and the shipped
`tools/native-suite.json` validation. `tools/lint.sh` passed shellcheck,
cppcheck and ruff.

`./build.sh HarnessChecks_APPL` built the controller, installer, acceptance
fixture and all registered checks with Retro68 in Docker:
`SherclawkWriteCheck`, `SherclawkSearchCheck`, `SherclawkEditCheck`,
`SherclawkInspectCheck`, `SherclawkProjectCheck`, `SherclawkScrollCheck`,
`SherclawkHandoffCheck`, `SherclawkSelfBuildCheck` and
`SherclawkSelfBuildStopCheck`. `suite.py validate --build build` accepted every
artifact; both self-build targets compiled with the private `buildjobs`
queue name.

A guest run of all the checks was not performed for this record: the host had
no mounted `Retro68` export or configured share host during verification.
Host tests prove the runner's completion, reset, evidence and no-replay rules;
the end-to-end guest qualification (including a MacRelix-free ToolServer run
and a fresh-boot reset between self-build checks) remains for the guest pass
before the epic closes.

# Reservation manager acceptance — October 22, 2026

Verified working tree on top of `c5657aa` (issue #71, ADR-0009). QEMU
10.1.2 `mac99` slots were booted concurrently from their immutable configured
images with QEMU from Homebrew; the `Retro68` export was mounted on the host
so fresh controller readiness could be read and stale staging could be
cleared. `tools/check.sh` (including the new reservation host tests),
`tools/check-transport.sh` and `tools/lint.sh` passed.

The opt-in acceptance ran
`OS9_HARNESS_SHARE="$HOME/Retro68Share" python3 harness/tests/guest_reservations.py`
and returned `ACCEPTED two-slot reservation manager`:

- Concurrent reserve calls returned distinct slots, distinct 128-bit
  tokens and distinct fresh 32-hex boot IDs; every QEMU process was alive
  after an advancing controller heartbeat.
- A further reservation was refused while every slot was held.
- Each slot produced a distinct live screenshot through the lease-checked QMP
  proxy.
- Releasing one slot revoked its lease first, stopped and verified the exact
  QEMU pid, reset its overlay, retained a JSON evidence record plus a PPM
  screenshot, and made the slot assignable again.
- Re-releasing the old token was a no-op; stale `renew`, `screenshot` and
  `input` calls failed after release and after reassignment, and the new
  owner's QEMU stayed alive.
- The reassigned slot had a later generation, a different pid and a different
  boot ID.
- Making the slot directory unwritable produced a failed overlay reset and
  quarantined the slot with a reason in `status`; the old QEMU had already
  been terminated. Restoring the directory and running `recover --slot`
  cleared the quarantine.
- An earlier interrupted boot had left `Harness/slot1/ready.tmp`; the manager
  removed that stale staging file before rebooting, and the slot published a
  fresh advancing heartbeat. The controller-side fix is tracked as #256.
- Every slot was free and stopped at the end. Evidence remains under
  `$OS9_HARNESS_HOME/evidence`, outside git.

Guest images and the raw acceptance evidence stay outside the repository.

# Publication transport probe — October 23, 2026

Verified working tree on top of `c8d983b` (issue #72, ADR-0009). `tools/check.sh`
(all C fault suites and the harness Python tests, including the new
publication suites) and `tools/lint.sh` passed.

The real transport was probed against the live AFP host without a guest:
`harness/publish.py install` published synthetic artifacts (a diagnostic
and `Sherclawk`) into a scratch `Harness/publishprobe*` directory with a
never-reused `/tmp` staging tree. An independent server-side inspection then
confirmed the data fork SHA-256, the `._` sidecar (AppleDouble v2 magic, exact
`Netatalk        ` filler, FinderInfo + resource entries), the 402-byte
Metadata xattr with the file's own dev/ino, and creator `????` for the
diagnostic versus `APPLShCk` for Sherclawk. Through the mounted export, the
guest-visible `com.apple.FinderInfo` read `APPL????`/`APPLShCk` and the
com.apple.ResourceFork bytes matched the local resource fork hash.

The server-side check failed closed on a removed sidecar and on a truncated
Metadata xattr, and a re-install repaired both to a clean check. The scratch
directory was then removed. End-to-end publication through a real reservation
with guest execution remains for #73/#74; this probe qualifies the transport.

# Native controller acceptance — October 22, 2026



Verified source: `b06d335`, based on main `813adf2` (issue #70, ADR-0009).
The six harness applications and shipping application built with Retro68 in
Docker using the user-supplied Universal Interfaces. `tools/check.sh` passed
its ASan/UBSan C suites and 115 stdlib Python harness tests;
`tools/check-transport.sh` and `tools/lint.sh` passed.

Both QEMU 10.1.2 `mac99` OS 9.2.2 slots installed the controller fork-aware on
the private boot disk, shut down cleanly, and were sealed into immutable
controller-configured slot images. Fresh overlays launched the controller from
Startup Items without Finder interaction. Each admitted exchange observed an
advancing heartbeat; recovery cases required a different boot ID. No API key,
Sherclawk agent session or MacRelix was used.

WriteCheck and InspectCheck ran concurrently in separate runtime workspaces,
reported named checks and exited before the controller accepted their passes.
All 13 opt-in acceptance cases returned the expected terminal outcome:

| Case | Slot | Outcome | Checks | Run ID |
|---|---|---|---:|---|
| active-stop | slot2 | stopped | 0 | 32b8561208b6ec236c6645e4f6901d56 |
| badconfig | slot1 | unknown | 0 | 2b70d945c125f32dd0d4fefd00a3223c |
| deadline | slot2 | unknown | 0 | 30cfc8b8a1059d5af89fbdbd0dae51b2 |
| duplicate | slot1 | passed | 1 | 62fc6b58be0c020329bdc7764d559761 |
| early | slot2 | unknown | 0 | 26218a958eb4a041f33a3a78bc509659 |
| fail | slot1 | failed | 1 | 38dc5c0a70362ad611c9d29b283bdd1e |
| identity | slot2 | harness_error | 0 | ece5e63a45b9286e9ddfbaaedadf1e61 |
| inspect | slot2 | passed | 113 | d23c2a21f64efc9b6a038d4c96e826a5 |
| partial | slot1 | harness_error | 0 | ac9749ba2714c8d73d0da0ae2c08e9b3 |
| pass | slot1 | passed | 1 | 546db4322a695731f0ad76d8af0f6ddb |
| stop | slot1 | stopped | 0 | d8dc89528095c86c19cf5a06f41c1767 |
| target | slot2 | harness_error | 0 | 9cac7c8cf62f77296ee93125e20b4ac6 |
| write | slot1 | passed | 11 | ac6bae869c7c86106994303c70a40e81 |

The duplicate case retained the original result unchanged and left the repeated
request unclaimed. Pre-launch Stop skipped process observation; active Stop
followed the returned PSN's observation phase and also produced a diagnostic
`stopped` report. Neither Stop result claims rollback or cancellation of earlier
operations. Quarantined guests were reset rather than reused or replayed.

Evidence remains under `Retro68:Harness:`: private `runs:<first 20 run hex>:`
fixtures contain the exact request and parsed `accepted.json`; archived slot
mailboxes retain claims, closed reports/results and bounded per-boot lifecycle
traces. Guest images and raw evidence are outside git. After verification both
runtime overlays were recreated from the immutable configured images and left
stopped, ready for the next boot.

Native qualification found and corrected use of Retro68 libc's unimplemented
`remove()`; all controller file removal now uses File Manager calls. One
partial-report attempt failed at launch and truthfully returned `unknown`;
it was retained and never replayed. Separate per-slot AFP application copies
then allowed the partial-report case to execute and return `harness_error`.
One boot stopped in Open Firmware: readiness admission withheld its request
until a successful fresh boot. Neither preliminary failure was counted as a
passing diagnostic.

Configured image SHA-256 values recorded by provisioning:

- slot1: `89f1347351d46c3d1612ac4044f14eae19ac229821c710e17c9e0f4b56a591a7`
- slot2: `461bbd28742fa8539ffdf8b388b48e5686f3e2f0f36c9104fd2dc93c18fc07b4`
- shared base: `09e445fbac01f30590d0445e8a1a6e3e424538bcd942981cdde413be1d502394`

Published application data-fork SHA-256 values matched the local build:

- `HarnessController.APPL`: `a8f321fb96f43266f20088c128ecf8f5731b6cc7187beeee68741b2dad76a491`
- `HarnessAcceptance.APPL`: `2a52b6c40d1c0a5b9042640ba97fa62ed85ec340fccbf2637ae37d9b10c9b895`
- `HarnessInstall1.APPL`: `7071d0fc40cc2ad857c1e8645899c102f0c4ba5044ac7e1698d4bbadad16da78`
- `HarnessInstall2.APPL`: `d744722b2a9a0d9071334e460b3b092be0015ba9b346904706170b3b1776d17a`
- `SherclawkWriteCheck.APPL`: `f9fc57f7746a8a21e235f5e725e2bbd8c605f942668b3af6e3a060e3cffb1153`
- `SherclawkInspectCheck.APPL`: `82a846d89c21bb49a74b8243e2954db096a7f9d3aa403048de2766496ae212c2`
