# OS 9 guest harness

Standalone QEMU guests for Mac OS 9 diagnostics. It is developed in this
repository and meant to be extracted later, so it imports nothing from
`tools/` or `src/` and keeps its own tests and documentation
([ADR-0009](../docs/adr/0009-build-harness-in-repo-then-extract.md)).

- `provision.py` — immutable base image, per-slot copy-on-write overlays,
  slot identities and the headless QEMU launcher.
- `guest.py` — the public command line for driving a guest over QMP:
  screenshots, keys, text and pointer control, for a slot, any QMP socket or
  the UTM VM. `qmp.py` (bounded protocol, input and capture), `screen.py`
  (framebuffer decoding) and `pointer.py` (pointer tracking) sit behind it.
  See [`docs/driving.md`](docs/driving.md).
- `native/` — Startup Items controller, cooperative run state machine, public
  diagnostic adapter and explicit guest failure fixture.
- `contract.py` and `mailbox.py` — version 1 request/report codecs and single-run
  CLI; [contract, installation and acceptance](docs/contract.md).
- `tests/` — stdlib-only host suites, including a fake QMP server, run by `tools/check.sh`; no QEMU, VM or
  network.
- [`docs/provisioning.md`](docs/provisioning.md) — how the guests were built,
  the exact emulator configuration and how to rebuild or change the base.

Disk images, installation media and the MPW distribution stay outside the
repository, under `$OS9_HARNESS_HOME`.
