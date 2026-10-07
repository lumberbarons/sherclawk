# Mac OS 9.2.2 on QEMU — testbed setup

The Sherclawk test guest: Mac OS 9.2.2 installed under QEMU on an Apple
Silicon Mac, with boot from disk verified end-to-end. This document records
the setup and the details worth knowing. It was originally written in the
imac workspace, where the VM lives; the VM folder, disk image and launcher
described below are not part of this repository.

## The VM folder

Outside this repository, the workspace keeps the VM laid out as:

```
macos922/
├── run-macos9.sh          # launcher — boots the installed system
├── macos9.img             # 8 GB (sparse) QEMU hard disk: macOS 9.2.2 installed
├── macos-922-uni/
│   ├── macos-922-uni.iso  # the installer CD (from your zip), for reinstalls
│   └── macos-922-md5.txt  # MD5 verified: 2cfb856b1678336493301bec0a0ecdfa
├── tools/                 # install automation used from the host (optional)
└── qemu-install.log       # log of the initial (automated) install run
```

Tested environment: macOS (Apple Silicon), QEMU 10.1.2 from Homebrew
(`brew install qemu`), machine `mac99` (PowerMac G4 class), 512 MB RAM,
640×480 display. The guest sees built-in Ethernet (sungem) and USB input.

## Running it

From the VM folder:

```bash
cd macos922
./run-macos9.sh
```

The launcher runs:

```bash
qemu-system-ppc \
  -M mac99 \                                    # PowerMac G4 "Sawtooth" class
  -m 512 \                                      # RAM — keep ≤ 1 GB for OS 9
  -rtc base=localtime \                         # classic Mac clock = local time
  -drive file=macos9.img,format=qcow2,media=disk \
  -device usb-kbd \
  -device usb-mouse \
  -nic user \                                   # user-mode networking
  -display cocoa,zoom-to-fit=on                 # window; resizable/scaled view
```

Notes:

- **First boot takes a minute or so** — PowerPC emulation is CPU-bound.
  Clicking/jiggling the mouse wakes the screen once the desktop loads.
- **Mouse grabs on click.** Ctrl+Alt+G releases it; Ctrl+Alt+F toggles
  full screen.
- **Setup Assistant**: on the first boot after the install you may get the
  Mac OS Setup Assistant wizard. It can be completed or simply quit
  (⌘Q → Quit) — it does not need to run.
- The startup volume is named **Macintosh HD**. Stock icons ("Browse the
  Internet", "Register with Apple", the Launcher strip at the bottom, …) are
  part of a normal 9.2.2 install and can be deleted freely.
- **Shut down from inside the guest**: Special → Shut Down. QEMU then exits
  by itself. Closing the QEMU window also ends the session.
- Prefer a GUI (and sound)? See **Running it in UTM** below.

## Running it in UTM (optional GUI route)

A UTM VM named **"Mac OS 9.2.2"** is set up in UTM.app. It runs the same
installed system as the command-line copy, but through UTM's GUI — and with
**working sound** (UTM bundles the `ppc-screamer` build of QEMU, which
emulates the Screamer audio chip that stock QEMU lacks).

What it uses:

- The community-maintained UTM template for Mac OS 9.2.2
  ([adespoton/utmconfigs](https://github.com/adespoton/utmconfigs)) — machine
  `mac99`, 512 MB RAM, `-cpu G3`, `-usbdevice keyboard` (UTM needs this for
  keyboard input), `via=pmu`, screamer audio, sungem networking.
- A **copy** of `macos9.img` inside the UTM VM's bundle, so it is independent
  of the command-line disk from here on. Changes made in UTM or via the CLI
  do not sync to each other; pick one for day-to-day use.

Caveats:

- **Required workaround (already applied):** UTM attaches `bootindex`
  properties to the VM's drives. On PowerPC, the OpenBIOS firmware then
  builds its boot list from full device paths and fails to find a Mac OS
  boot system — it loops printing `Trying …/disk:,\:tbxi…` forever. The fix
  is in that VM's arguments already: **Settings → System → QEMU Arguments
  contains `-prom-env` and `boot-device=disk`**. UTM detects the
  `boot-device=` argument and disables `bootindex` for PPC guests, after
  which the firmware uses its normal `hd:,\:tbxi` boot path. Do not remove
  these arguments. (Context: utmconfigs issue #74; UTM 4.7.5 release note
  "QEMU PPC: Disable bootindex when boot-device is used in prom-env".)
- Pause/snapshot are unavailable for PPC VMs in UTM (a side effect of the
  screamer audio support).
- If the VM ever refuses to start, disable Sound in its settings (some
  devices are picky about audio sample rates — see the config's own notes).

To recreate this setup from scratch:

```bash
# 1. Download the community config
curl -L -o /tmp/utm922.zip "https://github.com/adespoton/utmconfigs/raw/main/Mac%20OS%209.2.2%20(config%20only).utm.zip"
# 2. Unzip and move the bundle into UTM's library
unzip /tmp/utm922.zip -d /tmp
cp -R "/tmp/Mac OS 9.2.2 (config only).utm" \
      ~/Library/Containers/com.utmapp.UTM/Data/Documents/"Mac OS 9.2.2.utm"
# 3. Swap in your installed disk
cp macos9.img ~/Library/Containers/com.utmapp.UTM/Data/Documents/"Mac OS 9.2.2.utm"/Images/"Mac OS 9.2.2.qcow2"
# 4. In UTM: Settings -> System -> QEMU Arguments, add two entries:
#      -prom-env
#      boot-device=disk
#    (disables UTM's bootindexes, which otherwise prevent Mac OS from booting)
# 5. Open UTM.app and press Play on "Mac OS 9.2.2"
```

## Networking

`-nic user` gives Mac OS 9 a DHCP-friendly Ethernet interface (emulated Apple
GMAC/sungem card):

- Guest IP: `10.0.2.15`, gateway/host: `10.0.2.2`, DNS: `10.0.2.3`
- The guest can reach the internet through the host (subject to the usual
  limitation: OS 9 browsers can't speak modern TLS, so plain-HTTP sites and
  old-school services work best).
- To expose a guest service (e.g. a web server) to the host, add a port
  forward in the run arguments: `-nic user,hostfwd=tcp::8080-:80`

## Sound

The **command-line setup has no sound** — stock QEMU doesn't emulate Mac OS
9's Screamer audio chip. Options: use the **UTM VM** (sound works out of the
box there), or build the community `qemu-screamer` fork
(`mcayland/qemu`, branch `screamer`) from source for the CLI.

## Writing software for it

This repository is the cross-compiler setup (Retro68 GCC for PowerPC classic
Mac OS) and the validated publish workflow: the [README](../README.md) builds
Sherclawk and `tools/deploy-to-share.sh` publishes an app to the AFP share on
`$SHARE_HOST`; the guest auto-mounts the share at boot and runs apps straight
from it. The original workspace also kept small sample projects in
`hello-world/` and `hello-world2/` and a disk-image fallback,
`tools/deploy-to-utm.sh`.

## Common tasks

- **Attach a CD/DVD image** — add to the launcher:
  `-drive file=/path/to/image.iso,format=raw,media=cdrom`
- **Change screen resolution** — use the guest's *Monitors & Sound* control
  panel; QEMU's VGA drivers support several modes.
- **Transfer files in** — easiest is an ISO made on the host:
  `hdiutil makehybrid -o files.iso -hfs /path/to/folder`, attach it as a CD.
- **Reinstall from scratch** — remove `macos9.img`, create a fresh one
  (`qemu-img create -f qcow2 macos9.img 8G`), boot the installer CD:

  ```bash
  qemu-system-ppc -M mac99 -m 512 -rtc base=localtime \
    -drive file=macos9.img,format=qcow2,media=disk \
    -drive file=macos-922-uni/macos-922-uni.iso,format=raw,media=cdrom \
    -boot d -device usb-kbd -device usb-mouse -nic user -display cocoa
  ```

  In the live CD environment: **Utilities → Drive Setup** → select
  *&lt;not initialized&gt;* → *Initialize…* → confirm. Then run
  **Mac OS Install** from the CD window and follow the dialogs
  (Destination: untitled → Select → Continue through information and
  license ⇢ Agree → Start).

## Troubleshooting

| Symptom | Fix |
|---|---|
| Blank/static screen at boot | Wait up to 2 minutes; it's a G4 emulated in software |
| Mouse seems dead after launch | Click inside the QEMU window; Ctrl+Alt+G toggles grab |
| OS 9 hangs/panics | Lower RAM (`-m 256`) — never exceed 1 GB with OS 9 |
| "Startup disk will not work" | Make sure the machine is `-M mac99` |
| Installer shows no destination disk | Run Drive Setup and initialize the disk first |
| Boots the CD instead of the disk | Remove any `-boot d` / CD drive arguments |

## Appendix A — how this install was performed

The GUI installer was driven automatically from the host because Mac OS 9 has
no unattended-install path. The vendored `tools/qmpdrive.py` talks to QEMU's
QMP interface (mouse/keyboard events) and tracks the guest cursor with
framebuffer screenshots (screen differences + a template of the classic
arrow). The scripts initialized the disk via Drive Setup, launched
*Mac OS Install*, clicked through every dialog, and boot-from-disk was
verified afterwards. The automation came from the VM folder's `tools/`; the
QMP driver remains here for reference — day-to-day you only need
`run-macos9.sh`.

## Appendix B — alternative: pre-installed disk images

The Mac OS 9 community (Mac OS 9 Lives forum, E-Maculation wiki, mikeboss)
publishes ready-made QEMU disk images with OS 9.2.2 pre-installed. If you ever
want to skip the install entirely, download one of those and point the
`-drive file=…` at it — the QEMU flags above stay the same. Usual caution
applies to third-party disk images.