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
(all C fault suites, 207 harness Python tests including the new publication
suites) and `tools/lint.sh` passed.

The real transport was probed against the live AFP host without a guest:
`harness/publish.py install` published two synthetic artifacts (a diagnostic
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
