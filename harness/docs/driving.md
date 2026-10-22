# Driving a guest

`harness/guest.py` is the harness's public command line for QMP: screenshots,
keys, text and the pointer. Consumers call it (or `provision.py`) and never
import the modules behind it, so it can move to its own project unchanged
([ADR-0009](../../docs/adr/0009-build-harness-in-repo-then-extract.md)).

```bash
python3 harness/guest.py --slot slot1 screendump shot.png   # PNG or PPM by extension
python3 harness/guest.py --slot slot1 pclick 592 104        # closed-loop pointer
python3 harness/guest.py --slot slot1 key meta_l o
python3 harness/guest.py --slot slot1 type "Soft"
```

Choose the guest with `--slot` (a provisioned slot), `--socket` (or
`$QMP_SOCK`) or `--utm` (the UTM "Mac OS 9.2.2" VM). `guest.py --help` lists
the commands. `tools/utm_qmp.py` and `tools/guest-input.py` are thin wrappers
that run `guest.py --utm`, so the documented UTM workflow is unchanged.

## What it guarantees

- **Bounded.** Every QMP operation has a deadline (20 s by default), so a
  wedged guest, or a socket another client holds, is an error rather than a
  hang. The socket serves one client at a time: never shell out to `guest.py`
  from a process that already holds a connection.
- **Correlated.** Commands carry ids and only the matching response is
  accepted. Events are recorded; a late reply to a command that timed out is
  discarded instead of being taken for the next answer. A dropped connection
  raises and stays closed.
- **Whole text or nothing.** `type` maps the complete string to keys first and
  refuses it if any character has no key, so a partial string never reaches
  the guest.
- **Non-overlapping keys.** Each press is held 200 ms (the guest drops
  shorter ones) and followed by a release gap before the next. A click
  releases its button even when the hold fails.
- **Relative pointer found, not assumed.** Pointer commands read
  `query-mice` and, if the current device is not relative, `mouse_set` the
  relative one it lists (preferring a HID device). An absolute tablet ignores
  the relative moves the harness sends.
- **Checked screenshots.** Each capture asks for a unique file name and a
  stale file cannot stand in for it. A capture that is missing, older than the
  request, malformed or truncated is an error, the temporary file is always
  removed, and the output file is written atomically, so a failed capture
  leaves nothing behind.
- **Cursor state belongs to one VM run.** Template and position state is kept
  under `$TMPDIR/os9-guest-state` (or `--state-dir`), keyed by the socket. A
  remembered pointer position is used only for the incarnation of the VM that
  recorded it (identified by the socket file QEMU creates at start-up), and is
  dropped when the helper sees a guest `RESET` or runs `guest.py reset`. A
  guest that is reset while no helper is connected is not detected: run
  `reanchor`, or use the closed-loop commands, which do not use stored state.
- **Near first.** Locating the pointer searches around where it is expected
  before scanning the whole screen.

## Pointer commands

| Route | Commands | Use when |
|---|---|---|
| Closed loop | `pgoto`, `pclick`, `pdclick`, `phold`, `tap`, `nudge`, `release` | Default. Parks the pointer in the corner, keeps that screen as a baseline and finds the pointer as the arrow-sized blob that differs; works over dialogs, menus and text. |
| Template | `prepare`, `reanchor`, `locate`, `goto`, `click`, `dclick` | A calibrated arrow outline is located in screenshots. Mislocates over busy artwork. |
| Blind | `moveto`, `clicknow`, `dclicknow` | Menus, from the remembered position; verify with a screenshot before clicking. |

Menus need the button held: `phold` on the title, `nudge` down (about 28 moves
one item), screendump to confirm, then `release`. Keep a menu interaction in
one process, since a new connection that re-locates the pointer closes the
menu.

The display lags about one action, so wait a few seconds before a screendump.
