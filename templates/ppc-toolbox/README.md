# Native PowerPC Toolbox template

MrC → PPCLink → Rez, executed by MPW ToolServer on OS 9.2.2.
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
component exists. The source must remain within the 4096-byte edit limit.

## Runtime diagnosis

The starter creates `runtime.log` beside its executable, using the Process
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
to locate the log (`buildjobs` build folders appear as
`Worker01:buildjobs:<build_id>:build:native:runtime.log` relative to the Retro68
workspace). Runtime logs are separate from compiler
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
| `VERIFIED.md` | Exact tested installation and guest acceptance results |
| `../../tools/native-process-check.c` | Confirm actual classic application paths |
| `../../tests/template/test_template.c` | Host checks for `io.c`, `png.c` and `selfrender.c` against a modelled Toolbox |
| `../../tools/native-build-check.c` | Fixed native ToolServer build/launch and compiler-error diagnostics |

This is a starter. Its fixed sources, `app.r` and `Template` output are
defaults, not requirements for every future project. `create_project` embeds
its editable C/Rez sources; `build_project` also accepts independently
assembled protocol-2 projects with multiple sources/resources, a chosen output
name and supported toolchain settings. See the
[build/run contract](../../docs/architecture.md#buildrun-contract). Every build runs through
the app's native MPW ToolServer executor; the generated recipe is the
`mpw-ppc-v2` adapter in `build_project.c`.

The fixed native diagnostic also builds the embedded starter sources directly.
Its two successful builds, deliberately failing compile and Stop during
linking are recorded in [native executor evidence, increment 2](../../docs/history/native-executor-evidence.md#increment-2-guest-evidence--october-6-2026).
Those historical results predate the native-widget/runtime-log starter; see
`VERIFIED.md` for the scope of recorded guest evidence. `VERIFIED.md` also
records the earlier MacRelix-driven verification of this recipe; that driver
has been removed.

## Build and launch

Create the starter with `create_project` and build it with `build_project`;
success returns a build ID, and `run_application(build_id)` launches the
authorized artifact. Reproduce a compiler failure by replacing `main.c` with
`#error Sherclawk deliberate compiler failure` (MacRoman/CR) and building: the
result is a nonzero exit, a filename/line diagnostic and no artifact. Restore
the source and rebuild under a fresh build ID. Earlier build snapshots stay
untouched.

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

To confirm executor identity, build `native-process-check.c` as the `main.c` of
a copy of this starter (MacRoman/CR, output name of your choice) and launch it
through `run_application`. Its fixed diagnostic output is
`Retro68:Template01:runtime.log`; adapt that path for another fixture name. It
enumerates classic Process Manager paths. Restore the Toolbox source before
further builds.
