# Native PowerPC Toolbox template

MrC → PPCLink → Rez, executed by MacRelix and MPW ToolServer on OS 9.2.2.
The model can change the sources and `app.r`; the recipe supplies startup
libraries, the PEF executable, `cfrg`, the `SIZE` resource in `app.r` and Finder
`APPL/SHTP` metadata. The app redraws on updates, yields with `WaitNextEvent`,
and provides a Control Manager Clear button and a TextEdit field (255 bytes).
Mouse selection, typing, deletion and arrow keys use TextEdit; Clear empties
the field without quitting. The window can be dragged and quits on a confirmed
close-box click or Command-Q. Releasing outside the close box cancels the close.
Keep native components and their event, activation, redraw and disposal paths
when extending the starter. Use QuickDraw for custom content and decoration;
use custom interactive widgets only when requested or no suitable native
component exists. Every source file must remain within the 4096-byte edit
limit, so the starter is split by role (see the table below).

## Runtime diagnosis

The starter (`io.c`) creates `runtime.log` beside its executable, using the Process
Manager's application location rather than assuming a current directory. It is
a MacRoman/CR `TEXT`/`ttxt` data fork, reset on each launch and capped at 4096
bytes. Entries record startup, Clear actions, allocation failures with error
codes, and normal quit. They contain no field contents or secrets. Each entry
uses a bounded File Manager write, not buffered stdio; there is no idle/draw
logging. Reaching the cap drops later entries; open/write errors disable logging
without retries. An open failure changes the window title to `Logging unavailable`.

When diagnosing a generated app, extend these entries around relevant state
changes and failures, reproduce the problem, then read its runtime log through
`read_text`. Use the artifact's actual folder from the successful build result
to locate the log (for the recipe below, `Template01:build:b001:runtime.log`
relative to the Retro68 workspace). Runtime logs are separate from compiler
stdout/stderr and are evidence of recorded events, not a complete UI test.
Two applications in the same folder share this filename; choose distinct log
names if that is needed. Save evidence before relaunching because it resets.

For interaction verification in OS 9: type and select text, delete and move the
caret, click Clear, drag and uncover the window, switch away and back to check
caret activation, cancel a close-box click, then quit with Command-Q. Read the
log for startup/Clear/quit; repeat with a confirmed close-box click. A read-only
application folder should show `Logging unavailable` while leaving the UI usable.
Host checks validate embedding; `./build.sh SherclawkTemplate_APPL` compiles
the exact starter with Retro68. Neither substitutes for these guest checks.

| File | Purpose |
|---|---|
| `main.c` | Event loop, window and control setup, close-box/Command-Q handling |
| `scene.c` | Window state, the click hit test and `draw_scene`, the one drawing routine |
| `io.c` | `runtime.log` and other small files written beside the executable |
| `png.c` | Dependency-free PNG writer for 8-bit indexed pixels |
| `selfrender.c` | Offscreen render, `frame.req` polling and the `frame.ready` marker |
| `tmpl.h` | Shared declarations and the `SELF_RENDER` compile gate |
| `app.r` | `SIZE` resource |
| `build-native.sh` | Native recipe with a fresh directory per attempt |
| `capture-toolchain.sh` | Installed compiler, linker, Rez, ToolServer and SDK capture |
| `VERIFIED.md` | Exact tested installation and guest acceptance results |
| `../../tools/materialize-native-template.py` | Create new MacRoman/CR source and LF shell scripts |
| `../../tools/native-process-check.c` | Confirm actual classic application paths |
| `../../tests/template/test_template.c` | Host checks for `io.c`, `png.c` and `selfrender.c` against a modelled Toolbox |
| `../../tools/native-build-check.c` | Fixed native ToolServer build/launch and compiler-error diagnostics, with MacRelix quit |

This is a starter and acceptance fixture. Its fixed sources, `app.r`
and `Template` output are defaults, not requirements for every future project.
`create_project` embeds its editable C/Rez sources; `build_project` also
accepts independently assembled protocol-2 projects with multiple
sources/resources, a chosen output name and supported toolchain settings. See
the [build/run plan](../../PLAN.md#buildrun-contract). The shell recipe
below remains the verified fixed-template diagnostic workflow.

The fixed native diagnostic also builds the embedded starter sources without
MacRelix. Its two successful builds, deliberately failing compile and Stop
during linking are recorded in [idea 005, increment 2](../../docs/idea-005-sherclawk-queue-mode.md#increment-2-guest-evidence--october-6-2026).
Those historical results predate the native-widget/runtime-log starter; see
`VERIFIED.md` for the scope of recorded guest evidence.
The main app executes the same queued ToolServer commands directly when the
queue is unowned; the file-job worker remains the exclusive-owner fallback.

## Materialize and publish source

From the repository root:

```bash
python3 tools/materialize-native-template.py build/Template01
ssh "$SHARE_HOST" 'mkdir /tmp/Template01-src'
scp build/Template01/* "$SHARE_HOST":/tmp/Template01-src/
ssh "$SHARE_HOST" 'sudo -n mkdir /srv/retro68/Template01 && sudo -n chown macos9:macos9 /srv/retro68/Template01 && sudo -n install -o macos9 -g macos9 -m 666 /tmp/Template01-src/* /srv/retro68/Template01/'
```

Both destinations must be new; choose another project name if either exists.
Change host/share paths for another server. These are new plain source data
forks; the recipe sets TEXT in the guest. Compiled applications must later
be transported fork-aware. Git sources are LF; the materializer converts
C/Rez to MacRoman/CR and leaves shell scripts LF.

## Native build and launch

In **MacRelix**, with the Retro68 volume already mounted:

```sh
cd /Volumes/Retro68/Template01
sh capture-toolchain.sh > toolchain.log 2>&1
sh build-native.sh b001 > b001.log 2>&1
echo $?
```

Inspect status immediately. Success requires exit 0, a
`stage=complete status=0` line and `build/b001/success.txt` naming `Template`.
After success only:

```sh
open build/b001/Template
```

IDs contain 1–24 ASCII letters, digits, underscores or hyphens. Every attempt
reserves a new directory, copies its inputs there and compiles those copies.
Reusing an ID fails. The last stage-start line locates a failure; redirected
logs preserve raw ToolServer diagnostics. A later-stage failure can leave a
partial app, but cannot create a success record. Do not launch that app.
Source revision tracking and job polling are provided by the model-facing
build tools; broader recovery remains subsequent work.

The installed shell needs separate `set -e` and `set -u`; combined `set -eu`
and multiline linker continuations did not work in verification. The recipe
also avoids compound shell syntax and exit traps.
The recipe uses single-line MPW commands and Perl core for ID validation.
Literal double quotes inside shell single quotes protect MPW variables whose
expanded paths contain spaces. ToolServer may take foreground focus; refocus
MacRelix before typing. Its diagnostics arrive after each command finishes.

The capture script assumes the explicit SDK location in VERIFIED.md.
Adapt that path for another installation. `MPW_DIR` controls
discovery, not executor selection. MacRelix `/proc` lists its own tasks,
not classic applications.

## Self-render

Generated apps can draw their own content region into an offscreen 8-bit
`GWorld` and write it as a PNG, so Sherclawk can see what the app draws while
it is fully covered by Sherclawk's window and never has to be fronted. The
pixels live in the app's own memory; nothing reads the screen.

Drawing is port-relative. `draw_scene(GrafPtr, flags)` in `scene.c` is the only
drawing routine: the `updateEvt` path calls it with the window, and
`selfrender.c` calls it with the `GWorld`. For the offscreen pass it points the
TextEdit record's `inPort` and the control's `contrlOwner` at the offscreen
port and restores both afterwards; keep any new TextEdit or control drawing
inside `draw_scene` so it follows the same rule. Windows that draw other
content must take the port from the argument instead of assuming the window.

Protocol, all files beside the executable (the same folder as `runtime.log`):

| File | Written by | Meaning |
|---|---|---|
| `frame1.png` … `frameN.png` | the app | PNG of the content region, one per requested frame; stored (uncompressed) deflate, 8-bit palette from the offscreen color table |
| `frame.ready` | the app, last | One CR-terminated `TEXT` line: `seq=S status=ok frames=N w=W h=H file=frame1.png flags=F`, or `seq=S status=error code=E` |
| `frame.req` | Sherclawk (`write_text`, create-only) | Request for another frame sequence; the app deletes it when it starts |
| `runtime.log` | the app | `frame code=S` or `frame_error code=E` when a request finishes |

The first request is made by the app itself after its first window update.
`frame.ready` is deleted when a request starts and written only after every
frame of that request is on disk, so its presence means the newest request is
complete and a stale marker is never trusted. A failed render, file or
allocation error produces `status=error`; if even the marker cannot be written
it is absent and the code is in `runtime.log`. `seq` increases per accepted
request, so a caller can tell a new marker from the previous one. Requests are
polled twice a second from the event loop, work while the app is backgrounded,
and are ignored while one is running. A request file may contain these words,
in any order:

| Word | Effect |
|---|---|
| `overlay` | Draws the hit regions (text field and Clear button) in red |
| `selftest` | Runs the in-app checks and draws `selftest PASS` or `selftest FAIL` into the image; the same data is logged as `selftest code=<failed-check bits>` |
| `frames=N` | 2–8 frames, six ticks apart, serviced from the event loop so the app can animate between them; anything else is one frame |

The self-test (`scene.c`) is the template's example of making interaction bugs
visible in a still image: it sends the center of each hit region through the
same `region_at` the mouse handler uses (a transposed `SetRect` or a transposed
`Point` makes this fail) and then performs the edit transitions on a scratch
text field, so the visible text is never changed. Replace its checks with the
application's own state transitions, and add the app's own hit regions to the
overlay.

Gating: `SELF_RENDER` in `tmpl.h` is `1`. Set it to `0` to compile the
overlay, self-test and the request polling out of `draw_scene`, and remove
`png.c` and `selfrender.c` from `project.json` to free their input slots; the
`sr_ask`/`sr_poll` calls become empty macros. Updating the embedded template
does not change existing projects (`ppc-toolbox-v1` sources are copied when a
project is created), so only new projects get this.

Memory: the offscreen buffer is `width * height` bytes (8-bit, plus row padding)
and is allocated per frame and released before the next, with a `width + 6` byte
row buffer for the PNG; the file is written row by row, so nothing image-sized
is held besides the `GWorld`. The default 390 × 150 window needs about 58 KB.
`app.r` therefore asks for 2 MiB preferred and 1.5 MiB minimum, up from 1 MiB;
`docs/limits.md` has the budget. A window larger than the free heap fails with
`status=error code=-108` instead of crashing.

Evidence: `tests/template/test_template.c` decodes the PNG with an independent
parser (chunk CRCs, stored blocks, Adler-32, palette, pixels) and covers the
request protocol and failures against a modelled Toolbox. The offscreen draw of
controls and TextEdit, the occluded-window behavior and the memory figures are
guest-only and not yet verified; see `VERIFIED.md`.

## Repeat the compiler-error check

Preserve `main.c`, replace its contents with
`#error Sherclawk deliberate compiler failure` in MacRoman/CR, then build with
a fresh ID. Expect nonzero status, a filename/line diagnostic, and no
executable or success record. Restore the source, build under another new ID,
and launch that successful result. Earlier build directories stay untouched.

For executor identity, convert `native-process-check.c` to MacRoman/CR and
use it as `main.c` in an isolated copy of this template. Build and launch it.
Its fixed diagnostic output is `Retro68:Template01:runtime.log`; adapt that
path for another fixture name. It enumerates classic Process Manager paths.
Restore the Toolbox source before further builds.
