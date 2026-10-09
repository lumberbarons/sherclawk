# Native PowerPC Toolbox template

MrC → PPCLink → Rez, executed by MPW ToolServer on OS 9.2.2.
The model can change `main.c` and `app.r`; the recipe supplies startup
libraries, the PEF executable, `cfrg`, a 1 MiB `SIZE` resource and Finder
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
| `main.c`, `app.r` | Toolbox application with close-box/Command-Q handling and resources |
| `VERIFIED.md` | Exact tested installation and guest acceptance results |
| `../../tools/native-process-check.c` | Confirm actual classic application paths |
| `../../tools/native-build-check.c` | Fixed native ToolServer build/launch and compiler-error diagnostics |

This is a starter. Its fixed `main.c`, `app.r` and `Template` output are
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

To confirm executor identity, build `native-process-check.c` as the `main.c` of
a copy of this starter (MacRoman/CR, output name of your choice) and launch it
through `run_application`. Its fixed diagnostic output is
`Retro68:Template01:runtime.log`; adapt that path for another fixture name. It
enumerates classic Process Manager paths. Restore the Toolbox source before
further builds.
