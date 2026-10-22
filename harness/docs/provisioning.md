# Provisioning standalone OS 9 test guests

Guests are copy-on-write overlays on one immutable base image. The base is
Mac OS 9.2.2 from the original QEMU install plus the MPW distribution
(MPW Shell and ToolServer); nothing else is added, in particular not MacRelix.
Neither the original QEMU disk, its launcher nor the UTM virtual machine is
ever opened for writing.

Supply your own lawfully obtained Mac OS installation and MPW distribution;
this repository includes neither, and no disk image is ever committed.

## Where things live

State is kept outside every worktree, in `$OS9_HARNESS_HOME` (default
`~/.local/share/os9-harness`):

```
base/os9-original.qcow2   verbatim, read-only copy of the original QEMU disk
base/os9-mpw.qcow2        frozen base: original plus MPW (read-only)
base/golden-work.qcow2    writable working copy, present only while preparing a base
slots/<slot>.qcow2        overlay on the base, one per slot
run/<slot>.{qmp,pid,log}  QMP socket, pid file and QEMU log
manifest.json             emulator version and base/original checksums
```

`provision.py info` prints the emulator version, the manifest and every slot
identity. `provision.py launch` refuses to start a guest if the base no longer
matches the manifest checksum.

## Emulator and baseline

Recorded configuration (macOS on Apple Silicon, QEMU from Homebrew):

- `qemu-system-ppc` 10.1.2, machine `mac99`, 512 MB, `-rtc base=localtime`.
- `-device usb-kbd -device usb-mouse` (relative pointer), no sound, no
  display (`-display none`; `--display cocoa` opens a window instead).
- A `sungem` NIC on user-mode networking, `-nic user,model=sungem,mac=<slot mac>`.
- The OS 9 desktop is 640×480 on the first headless boot of the original disk;
  no `-g` option is passed.
- The original launcher (`run-macos9.sh`) used the same machine, memory, clock
  and input devices, so the baseline is unchanged. It left the NIC model to the
  default; slots name `sungem` explicitly, the card the UTM virtual machine
  uses. Change a baseline value only after a guest has verified the change.

The original disk's checksum is recorded in `manifest.json` as
`original_sha256` (SHA-256 `1f9dd4a3…` for the install used here).

## Slot identities

A slot is a name with fixed identity values derived in `provision.py`:

| Value | Rule |
|---|---|
| overlay | `slots/<slot>.qcow2`, backed by `base/os9-mpw.qcow2` |
| MAC address | `52:54:00:09:00:NN`, locally administered, `NN` the slot number |
| QMP socket | `run/<slot>.qmp`, one client at a time |
| pid / log | `run/<slot>.pid`, `run/<slot>.log` |

Each guest has its own user-mode network stack: every guest sees `10.0.2.15`,
gateway `10.0.2.2`, and they cannot reach each other directly. Both reach the
AFP share host over its LAN address and hold independent AFP sessions
(`info usernet` shows one established connection to port 548 per guest). Guests
exchange data only through the share; add `hostfwd` rules per slot if a service
must be reachable from the host.

## Building the base

Run from the repository root. `HOME_DIR` is `$OS9_HARNESS_HOME`.

1. Preserve the original and make a working copy. Never touch the source disk:

   ```bash
   cp macos922/macos9.img "$HOME_DIR/base/os9-original.qcow2"
   chmod a-w "$HOME_DIR/base/os9-original.qcow2"
   cp "$HOME_DIR/base/os9-original.qcow2" "$HOME_DIR/base/golden-work.qcow2"
   chmod u+w "$HOME_DIR/base/golden-work.qcow2"
   python3 harness/provision.py launch-work slot1
   ```

   The guest reaches the desktop in about two minutes. If the original disk was
   last stopped uncleanly, a Disk First Aid notice appears first; press Return.

2. Mount the share and fetch MPW. In the guest: Apple menu > Chooser >
   AppleShare > Server IP Address…, enter the share host's address, connect as
   Guest, select `Retro68` and tick it to open at startup. The mounted volume
   then reappears on every boot. Drag `Software:MPW-GM.img.bin` onto
   Macintosh HD.

3. Decode it. `MPW-GM.img.bin` is a MacBinary-encoded Disk Copy image. Open
   `Applications (Mac OS 9):Internet Utilities:Aladdin Folder:Stuffit Expander 6.0`
   and drag the file onto the *Stuffit Expander application icon*; dropping it on
   the folder window only moves it. This produces `MPW-GM.img`.

4. Mount it. Double-click `MPW-GM.img`. The first use shows the Disk Copy
   software license, which has to be accepted by whoever owns the Mac OS
   installation. The `MPW-GM` volume appears on the desktop and holds the
   folders `MPW` and `Interfaces&Libraries`.

5. Install it. Drag the `MPW-GM` volume icon onto Macintosh HD. The copy lands as
   `Macintosh HD:MPW-GM:MPW-GM:` with `MPW` (MPW Shell, ToolServer, Tools,
   Startup) and `Interfaces&Libraries` inside. No installer runs and nothing is
   registered by hand: ToolServer is found through the desktop database, as the
   native build diagnostics expect. Put the `MPW-GM` volume away (select, then
   Command-Y).

6. Make slot boots unattended. Apple menu > Control Panels > General Controls:
   untick *Check disk if computer was shut down improperly*. The panel is taller
   than the 640×480 screen; drag it up by its title bar. Without this, every
   slot stopped with SIGTERM starts with the Disk First Aid notice.

7. Verify the toolchain (see below), then Special > Shut Down. QEMU exits when the
   guest powers off. Do not stop a base-preparation guest with SIGTERM.

8. Freeze and create the slots:

   ```bash
   python3 harness/provision.py freeze      # working copy -> base/os9-mpw.qcow2, read-only
   python3 harness/provision.py init slot1
   python3 harness/provision.py init slot2
   python3 harness/provision.py launch slot1
   python3 harness/provision.py launch slot2
   ```

   `freeze` refuses to run while a base exists, a guest is running or an overlay
   still names the base: replacing a backing file under an overlay corrupts it.

## Verifying a guest

Build and publish the fixed native build diagnostic, then launch it from the
`Retro68` window in each guest and read its log from the share (the log is
overwritten per launch):

```bash
SHARE_HOST=<afp-host> APP=SherclawkNativeBuildCheck tools/deploy-to-share.sh
ssh <afp-host> 'cat /srv/retro68/SherclawkNativeBuildCheck.log' | tr '\r' '\n'
```

A pass is `RESULT failures=0 fixed_native_build=complete`, with MrC, PPCLink and
Rez each `stat=0` through ToolServer and no MacRelix in the process list. Quit
the launched `Template` fixture and the diagnostic with Command-Q afterwards.

The base and slots described here passed on October 21, 2026 (details in the
[verification history](../../docs/history/verification.md)).

## Headless screenshots and input

With `-display none` the framebuffer is still readable and QMP input still
reaches the guest:

- `screendump` through QMP (`human-monitor-command` with `screendump <file>`)
  writes a PPM of the current 640×480 desktop.
- Key events work with a hold of about 200 ms; shorter holds are dropped.
- The pointer is relative. A slam to the corner followed by closed-loop tracking
  from screenshots reached menu items, icons and drag targets reliably; fixed
  blind moves did not.
- The display lags about one action; wait a few seconds before a screenshot.
- Keep a menu interaction in one QMP session: a new connection that first
  re-locates the pointer closes any open menu.
- The socket serves one client at a time.

This session used the helpers in `tools/` for that driving; consolidating them
into the harness is a separate piece of work (hew #69).

## Changing the base later

Examples: installing the native controller in Startup Items
(`Macintosh HD:System Folder:Startup Items`), or updating MPW. The base is
never edited in place:

```bash
python3 harness/provision.py reset slot1     # overlays must not outlive the base they name
python3 harness/provision.py reset slot2
cp "$HOME_DIR/base/os9-mpw.qcow2" "$HOME_DIR/base/golden-work.qcow2"
chmod u+w "$HOME_DIR/base/golden-work.qcow2"
python3 harness/provision.py launch-work slot1   # change the guest, then Special > Shut Down
mv "$HOME_DIR/base/os9-mpw.qcow2" "$HOME_DIR/base/os9-mpw.<date>.qcow2"
python3 harness/provision.py freeze
python3 harness/provision.py init slot1
python3 harness/provision.py init slot2
```

Keep an archived base only as long as it is useful; it is a full image.
