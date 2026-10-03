# Native PowerPC Toolbox template

MrC → PPCLink → Rez, executed by MacRelix and MPW ToolServer on OS 9.2.2.
The model can change `main.c` and `app.r`; the recipe supplies startup
libraries, the PEF executable, `cfrg`, a 1 MiB `SIZE` resource and Finder
`APPL/SHTP` metadata. The app redraws on updates, yields with `WaitNextEvent`,
and quits on a content click or Command-Q.

| File | Purpose |
|---|---|
| `main.c`, `app.r` | Toolbox application and resources |
| `build-native.sh` | Native recipe with a fresh directory per attempt |
| `capture-toolchain.sh` | Installed compiler, linker, Rez, ToolServer and SDK capture |
| `VERIFIED.md` | Exact tested installation and guest acceptance results |
| `../../tools/materialize-native-template.py` | Create new MacRoman/CR source and LF shell scripts |
| `../../tools/native-process-check.c` | Confirm actual classic application paths |

## Materialize and publish source

From the repository root:

```bash
python3 sherclawk/tools/materialize-native-template.py sherclawk/build/Template01
scp sherclawk/build/Template01/main.c sherclawk/build/Template01/app.r sherclawk/build/Template01/build-native.sh sherclawk/build/Template01/capture-toolchain.sh beardmore:/tmp/
ssh beardmore 'sudo -n mkdir /srv/retro68/Template01 && sudo -n chown macos9:macos9 /srv/retro68/Template01 && sudo -n install -o macos9 -g macos9 -m 666 /tmp/main.c /tmp/app.r /tmp/build-native.sh /tmp/capture-toolchain.sh /srv/retro68/Template01/'
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
Source revision tracking, job polling and recovery are subsequent work.

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
