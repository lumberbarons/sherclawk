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
