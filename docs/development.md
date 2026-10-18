# Building, deploying and checking Sherclawk

Run the commands in this guide from the repository root. Host checks use
Toolbox stubs; a successful host run does not establish guest behavior.
[AGENTS.md](../AGENTS.md) contains contributor conventions and the source map.

## Build and publish

Prerequisites: Docker with the `ghcr.io/autc04/retro68` image, host Python 3
(stdlib only) for `build.sh` and the artwork converter, and `ssh`/`scp` for
publishing, plus a clone of the
[`lumberbarons/certainly`](https://github.com/lumberbarons/certainly) fork next
to this repository, your own lawfully obtained Apple Universal Interfaces 3.4
SDK, and the vendored `art/` in this tree. Publishing needs an AFP
share host reachable over `ssh`/`scp` with passwordless `sudo -n`, a `macos9`
user and group, python3 and netatalk; files go to `/srv/retro68` unless
`SHARE_DIR` overrides it. The converter reads
`art/sherclawk.png` (8-bit RGBA, noninterlaced); override its path with
`SHERCLAWK_ART`.

```bash
git clone --depth 1 https://github.com/lumberbarons/certainly.git ../Certainly
# Supply your own SDK; this is the Interfaces&Libraries folder, not its parent.
export INTERFACES_DIR="/path/to/Interfaces&Libraries"
# Optional compiled fallbacks; normal runtime keys can be entered in Preferences.
cp config.example.h config.local.h
# Set SHERCLAWK_API_KEY and optionally SHERCLAWK_MODEL / SHERCLAWK_WORKSPACE.
./build.sh
SHARE_HOST=<afp-server> tools/deploy-to-share.sh
```

The build and every host check run against the commit pinned in
`tools/certainly.sh`. The script resolves the clone, moves a clean
one to the pinned commit (fetching it by SHA when the clone lacks it), and
refuses a checkout with local changes rather than modifying it; CI fetches
exactly the pinned SHA. A push to the fork therefore cannot change what
Sherclawk compiles against — taking a new Certainly version is a deliberate
PR: `tools/certainly-bump.sh` checks the fork's main out, runs
`tools/check.sh` and `tools/check-transport.sh` against it and rewrites the
pin (`--check` only reports a moved fork). Host suites do not cover guest
behavior, so a bump whose fork commits change runtime behavior still needs
its own OS 9 verification before merging.

### SDK setup

The SDK is not included or downloaded by this repository. Obtain it yourself
under its applicable license. `INTERFACES_DIR` must contain
`Interfaces/CIncludes/OpenTransport.h`, `Interfaces/RIncludes/`, and the
SDK's `Libraries/SharedLibraries/` and `Libraries/PPCLibraries/` directories
(including `OpenTransportAppPPC.o`). Preserve the libraries' resource forks
or their AppleDouble/MacBinary representations. Alternatively, place
the complete `Interfaces&Libraries` folder at `../InterfacesAndLibraries`
and leave `INTERFACES_DIR` unset. `build.sh` mounts that local directory
read-only and stages its headers and import libraries into the build container.
The open-source Multiversal Interfaces supplied by Retro68 lack the Open
Transport headers needed by Certainly.

### Optional build configuration

`config.local.h` is ignored, and credentials are embedded only in local
binaries. For a distributable build, omit `config.local.h` and let users enter
their own key in Preferences. Builds without credentials still launch. Quit a
running copy before publishing/relaunching. Normal runtime uses no host executor
or model relay.
Model requests identify the client as `Sherclawk/1.0 (Certainly; Mac OS 9)`;
Sherclawk's build defines `SHERCLAWK_APP`, which selects that User-Agent in the
vendored POST builder.
Set `SHERCLAWK_APP_URL` in `config.local.h` to a public app URL to also send
OpenRouter's `HTTP-Referer` and `X-OpenRouter-Title: Sherclawk` attribution
headers; an empty value sends neither.

### Host checks

Host checks need no Docker, VM or API key; the network suites need the
Certainly clone above:

```bash
tools/check.sh
tools/check-transport.sh
tools/lint.sh
tools/check-coverage.sh  # LLVM line/branch coverage gate; needs clang
```

Set `CERTAINLY_DIR` if the clone is elsewhere. Without Certainly,
`tools/check.sh` skips its network suite and `tools/check-transport.sh` fails.
The host checks compile shared code with `cc` and regenerate
`build/project-template.h`. The lint gate uses shellcheck, cppcheck and Ruff;
CI installs all three.

`tools/check-coverage.sh` measures line and branch coverage of the sources the
host suites compile, using LLVM source-based coverage. It needs `clang` plus
matching `llvm-profdata`/`llvm-cov` (macOS Command Line Tools, or
`apt install clang-18 llvm-18`), runs the same `tools/check.sh` suites under
`CHECK_COVERAGE=1`, and merges every suite into one per-file report. Coverage
is the union over suites; the gate fails below the checked-in floors (92%
lines, 67% branches over non-test sources; measured baseline 93.6% / 69.2%).
Guest-only sources that cannot compile on the host (`src/app/main.c`) and staged
Certainly code are excluded. Branch coverage is the emphasis because the
suites exist for command and error paths; the report lists the weakest files
first. Raise the floors deliberately.

Full builds also regenerate `build/art.r`, so use
`build.sh` rather than configuring CMake by hand in a fresh tree.

### Cost table and guest calibration

`tools/check.sh` ends with a table of deterministic cost counters, and
`tools/check-transport.sh` prints the TLS ones. Each suite reports its counters
through `tests/cost.h`, which also asserts the measured total against a bound
pinned to today's value, so a change that adds model rounds, event turns,
File Manager calls or network steps fails the host checks instead of only
showing up as slower guest timing. When a change lowers a total, lower its
bound in the same commit. The counters are:

| Counter | Reference |
|---|---|
| `jobs stage ...` (`tests/test_jobs.c`) | Event turns and File Manager calls to stage and publish a job at the snapshot input limits, plus the most File Manager calls in one idle poll turn |
| `build_project stage ...` (`tests/test_build_project.c`) | The same from a `build_project` call with a source set near the snapshot cap |
| `run_application launch verification` (`tests/test_quit_application.c`) | Event turns and File Manager calls from `run_application` to a verified, launched artifact |
| `agent model rounds` (`tests/test_agent.c`) | Model rounds for a run with one truncation retry and a fixed number of single-call tool rounds |
| `network_step turns` (`tests/test_network.c`) | `network_step` calls to send a 256 KiB request and receive a 64 KiB response in 1.4 KiB and 16 KiB records |
| `TLS pumps ...` (`tests/test_tls_io.c`) | `MacTLS_Pump` calls for the same transfer against the patched Certainly sources |

Host counts convert to guest time with the per-call costs that
`SherclawkCalibrateCheck` (`tools/calibrate-check.c`) measures on the real
`Retro68:` AFP volume. It runs unattended, writes
`Retro68:SherclawkCalibrateCheck.log`, deletes its scratch file and quits;
launch it with the guest otherwise idle. Each figure is total ticks over many
repetitions, because a tick is 1/60 s.

| Operation | Ticks per call, range over runs |
|---|---|
| `FSpOpenDF` + `FSClose` | 0.68 to 0.73 |
| `SetFPos` + 1 KiB `FSRead` on an open file | 0.33 to 0.37 |
| `FSpOpenDF` + 1 KiB `FSRead` + `FSClose` | 1.0 to 1.1 |
| `PBGetCatInfoSync` lookup | 0.32 to 0.37 |
| `SetFPos` + 1 KiB `FSWrite` on an open file | 0.38 to 0.43 |
| `FlushVol` | 0.35 to 0.45 |
| `FSpCreate` + `FSpDelete` | 3.1 to 4.1 |
| Event turn, `WaitNextEvent` sleep 0 | 0.02 to 0.14 |
| Event turn, sleep 1 (the busy-run cadence) | 1.0 to 1.1 |
| Event turn, sleep 10 (the idle cadence) | 8.4 to 8.6 |

The figures were recorded on October 16, 2026; the [verification
history](history/verification.md) has the raw totals. A busy run costs at least
a tick per event turn however little each turn does, and the File Manager calls
inside the turns add to that. Treat the conversion as a lower-bound estimate
for ordering changes, not a latency promise: the file figures repeat one 1 KiB
file that the share keeps hot, while staging touches many files and larger
transfers, and the runs were on one idle UTM guest and one AFP share. The
spread between runs is up to about 15% for most rows and wider for file
creation and the sleep-0 turn, so quote a range rather than one value. Rerun the diagnostic after
a change of host, share or guest configuration and update the table.


### Deployment

The default Docker target is `Sherclawk_APPL`; pass another CMake target as the
first argument, for example `./build.sh SherclawkProbe_APPL`. Output is in
`build/`: `.bin` is MacBinary (both forks), `.APPL` needs fork-aware transfer,
and `.dsk` is a generated disk image. The SDK tree and guest OS are not packaged.

Set `SHARE_HOST` to your AFP host before deployment or the diagnostic commands:

```bash
export SHARE_HOST="<afp-host>"
tools/deploy-to-share.sh
```

Quit the running app before republishing. Never restart AFP/netatalk during a
guest session. If a published app reports that it is in use, check for a stale
server-side AFP session with no live client before changing the guest; see
[the deployment notes](../AGENTS.md#deploying-to-the-guest).

## Application icon and resource forks

The Finder icon ships as a bundle icon family and as an attached custom icon,
so the AFP file displays correctly without rebuilding the Desktop database.
Both 16px and 32px icons have color, masks and monochrome fallback. The native
window draws a 156px RGB resource generated from the same image. Fork-aware
publication keeps the exact netatalk filler/table layout, with Sherclawk's
creator and custom-icon/bundle flags in Finder info.

## Optional live host probe

The host probe performs a real provider exchange and needs network access and
an API key in ignored `config.local.h`. It uses the
[host TLS shim](../vendor/host-tls/README.md); its file executor is simulated and
explicitly labeled, so it does not establish native File Manager behavior.
The normal host suites above need no key.

```bash
tools/build-host-probe.sh
build/host-probe
```

## Files and verification

[AGENTS.md](../AGENTS.md#layout) lists the source layout. Each `tools/*-check.c` is a Mac GUI
diagnostic built as its own CMake target; launch it in the guest and read
`Retro68:<Name>.log`:

| Diagnostic | Exercises |
|---|---|
| `SherclawkProbe` (`tools/probe.c`) | Real model, tool and follow-up conversation; host variant via `tools/build-host-probe.sh` |
| `SherclawkWriteCheck`, `SherclawkEditCheck`, `SherclawkSearchCheck`, `SherclawkInspectCheck` | The matching tool executors against the File Manager; `SherclawkInspectCheck` also covers `get_capabilities` and `get_volume_info` |
| `SherclawkViewImageCheck` | `view_image` against the File Manager: a PNG at the size cap read in bounded steps, refusals and the vision gate |
| `SherclawkMCPCheck` (`tools/mcp-check.c`) | MCP configuration, TLS initialize, discovery and a Tavily search; see [the MCP guide](mcp.md#protocol-diagnostic) |
| `SherclawkTrashCheck` | `move_to_trash` against the real volume Trash: what `FindFolder` resolves, a pinned rename, destination identity, recovery and the fallback |
| `SherclawkCatalogCheck` (`tools/catalog-check.c`) | `fs_catalog` against the real volume: the caller's `FSSpec` name buffer, poisoned past its length, comes back byte-identical for a file and a directory, with the canonical and a case-flipped name |
| `SherclawkCopyCheck` (`tools/copy-check.c`) | `copy_file` against the real volume: a fixture with both forks and Finder identity, a pinned publish under a new name, byte verification of both forks, an untouched source and the overwrite refusal |
| `SherclawkMCPGuardCheck` (`tools/mcp-guard-check.c`) | The MCP configuration guard against the real Preferences folder: the workspace is pointed at the boot volume, every protected tool is refused, listing and search skip the file, an alias target is refused, and its size and modification date stay unchanged; the log carries only statuses |
| `SherclawkProjectCheck` | `create_project` publication, bytes and metadata |
| `SherclawkCalibrateCheck` (`tools/calibrate-check.c`) | Guest ticks per File Manager call and event turn, for converting the host [cost table](#cost-table-and-guest-calibration) to time |
| `SherclawkBuildCheck`, `SherclawkRunCheck`, `SherclawkSelfBuildCheck`, `SherclawkSelfBuildStopCheck` | `build_project` / `run_application` through the native executor (`tools/build-check.c`) |
| `SherclawkToolServerCheck`, `SherclawkNativeBuildCheck`, `SherclawkNativeBuildErrorCheck` | ToolServer channel and fixed native build, described below |
| `SherclawkHandoffCheck` | Save Handoff persistence and seeding |
| `SherclawkScrollCheck` | Toolbox scrollbar behavior |

Host-side tests are in `tests/`, compiled against the stubs in `tests/toolbox/`
by `tools/check.sh`; the starter's `io.c`, `png.c` and `selfrender.c` have their
own Toolbox model in `tests/template/`; `tools/lint.sh` adds shellcheck, cppcheck and ruff for the
same sources, and `.github/workflows/ci.yml` runs those checks on `ubuntu-24.04`
for pushes to main and pull requests. Python helpers: `tools/make-art.py`
(icon and window art), `tools/netatalk_meta.py` (fork-aware publication),
`tools/embed-project-template.py` (starter template),
and `tools/guest-input.py`, `tools/qmpdrive.py` and `tools/utm_qmp.py`
(drive the UTM guest over QMP).

```bash
APP=SherclawkHandoffCheck tools/deploy-to-share.sh
# Launch in OS 9; inspect Retro68:SherclawkHandoffCheck.log.
APP=SherclawkProjectCheck tools/deploy-to-share.sh
# Launch in OS 9; inspect Retro68:SherclawkProjectCheck.log.
APP=SherclawkRunCheck tools/deploy-to-share.sh
# Launch in OS 9; inspect Retro68:SherclawkRunCheck.log.
./build.sh SherclawkProbe_APPL
APP=SherclawkProbe tools/deploy-to-share.sh
# Launch SherclawkProbe in OS 9; it logs, then quits.
ssh "$SHARE_HOST" 'cat /srv/retro68/SherclawkProbe.log'
APP=SherclawkWriteCheck tools/deploy-to-share.sh
# Launch SherclawkWriteCheck in OS 9; fixed diagnostic logs, then quits.
ssh "$SHARE_HOST" 'cat /srv/retro68/SherclawkWriteCheck.log'
APP=SherclawkSearchCheck tools/deploy-to-share.sh
# Launch SherclawkSearchCheck in OS 9; retains a unique source/backup fixture.
ssh "$SHARE_HOST" 'cat /srv/retro68/SherclawkSearchCheck.log'
APP=SherclawkEditCheck tools/deploy-to-share.sh
# Launch SherclawkEditCheck in OS 9; preserves its unique fixture and backups.
ssh "$SHARE_HOST" 'cat /srv/retro68/SherclawkEditCheck.log'
APP=SherclawkInspectCheck tools/deploy-to-share.sh
# Launch SherclawkInspectCheck in OS 9; retains alias/resource fixtures.
ssh "$SHARE_HOST" 'cat /srv/retro68/SherclawkInspectCheck.log'
APP=SherclawkMCPGuardCheck tools/deploy-to-share.sh
# Launch SherclawkMCPGuardCheck in OS 9; it logs statuses only, never file content.
ssh "$SHARE_HOST" 'cat /srv/retro68/SherclawkMCPGuardCheck.log'
APP=SherclawkTrashCheck tools/deploy-to-share.sh
# Launch SherclawkTrashCheck in OS 9; retains its fixture folder and leaves the
# moved item visible in the Trash. Record the resolution NOTES and RESULT line.
ssh "$SHARE_HOST" 'cat /srv/retro68/SherclawkTrashCheck.log'
APP=SherclawkCopyCheck tools/deploy-to-share.sh
# Launch SherclawkCopyCheck in OS 9; retains its fixture folder. Record the
# RESULT line before advertising copy_file.
ssh "$SHARE_HOST" 'cat /srv/retro68/SherclawkCopyCheck.log'
```

The cursor matcher can mistake highlights in the lobster artwork for the arrow.
After using UTM, check `info mice`; this VM needs `mouse_set 4` for the
working relative HID mouse. The active absolute tablet ignores relative input.
For UI diagnostics prefer the closed-loop pointer commands in `tools/utm_qmp.py`
(`pgoto`, `pclick`, `pdclick`, `phold`, `nudge`, `release`). They park the pointer
in the corner, keep that screen as a baseline and find the pointer as the
arrow-sized blob that differs from it, so they work over dialogs, menus and text
where the template matcher loses the arrow, and they end within a couple of
pixels whatever the gain. Menus and popups need the button held: `phold` on the
title, `nudge` down (a nudge of about 28 moves one item), screendump to confirm
the highlighted item, then `release`. Only one process may hold the QMP socket:
a script that already has a connection must call its own `screendump` rather
than shelling out to `utm_qmp.py screendump`, which would wait on the greeting.
Keys are held 200ms with a release gap, since the guest drops shorter presses
(Escape included). The older route (reanchor, blind `moveto`, verify a
screenshot, then `tools/guest-input.py --click` with the pointer still) remains.

The host probe labels its simulated file executor explicitly; it checks actual
provider protocol and transport. The guest probe uses actual native tools and a
fixed `Sherclawk Fixture.txt`, plus path, binary-file, duplicate-argument and
unknown-tool checks. Only fixed diagnostic conversations are printed. The write diagnostic preserves
a unique fixture folder; its log includes flushed mutation recovery records.

## Native build diagnostics

### ToolServer diagnostic

`SherclawkToolServerCheck` is a standalone asynchronous Apple-event spike.
The main app's `build_project` executes through this same queued ToolServer
channel. Publish the diagnostic with:

```bash
APP=SherclawkToolServerCheck tools/deploy-to-share.sh
```

Run only with ToolServer idle and no build in progress. It finds a running
`MPSX` process or discovers and launches ToolServer through mounted volumes'
desktop databases. It creates a fresh `Retro68:ToolServerCheck<ticks>:` fixture
with native `TEXT`/CR Rez sources and writes
`Retro68:SherclawkToolServerCheck.log` (overwritten at each launch). Save that
log before another launch; fixture folders and resource outputs are retained.

The automatic suite sends successful and deliberately failing Rez commands via
`misc/dosc`, `typeChar`, `kAEQueueReply | kAENeverInteract`. It verifies the
successful `STR `/128 resource, logs raw `stat`, stdout and diagnostics, and
checks wrong return IDs/senders, missing status, wrong text type and oversized
text using synthetic local events through the actual reply handler. The log's
`RESULT failures=0` applies to this suite; interactive fault tests have separate
records. Raw MacRoman/CR reply bytes remain in the log.

| Key | Diagnostic action |
|---|---|
| `R` | Explicitly start a fresh success/error suite when no command is outstanding |
| `L` | Send one script containing 41 fixed Rez invocations for interaction tests |
| `S` or Command-period | Stop observing; retain and drain the outstanding reply, keeping outcome unknown |
| `T` | Expire the outstanding command's deadline; drain its late reply without advancing |
| `K` | Politely request ToolServer quit (it may defer until the command finishes) |
| Command-Q | Quit the diagnostic, recording any outstanding command as unknown |

There is one outstanding command at a time and no automatic resend. If
ToolServer disappears, the request is retired as unknown; `R` may then test a
fresh launch. A reply whose sender cannot be verified is ignored. A lost reply
from a still-running server keeps fresh tests disabled until diagnostic exit.
Replies are capped at 8 KiB per text parameter for extraction; this does not cap
the Apple Event Manager's allocation of the incoming event itself. Logs include
send duration, elapsed ticks, event-loop turns, updates and maximum turn gap.
The window can be dragged, but ordinary Toolbox `DragWindow` tracking can defer
reply handling; production code must account for this before promising latency.
This spike does not build C projects or claim queue jobs.

### Integrated self-build diagnostics

Build/publish the integrated error–repair–rebuild and authorized-launch fixture:

```bash
APP=SherclawkSelfBuildCheck tools/deploy-to-share.sh
APP=SherclawkSelfBuildStopCheck tools/deploy-to-share.sh
```

Run with the queue unowned and ToolServer idle. It checks independent and starter
projects with multiple sources, explicit native ownership, resource structure,
compiler failures, revision-bound edits and fresh build IDs. Evidence is recorded
in `Retro68:SherclawkSelfBuildCheck.log`; acceptance is recorded in the
[verification history](history/verification.md).

### Fixed native build diagnostic

With ToolServer idle, publish and launch:

```bash
APP=SherclawkNativeBuildCheck tools/deploy-to-share.sh
APP=SherclawkNativeBuildErrorCheck tools/deploy-to-share.sh
```

Launch each separately in Finder and save its root log before repeating it:
`Retro68:SherclawkNativeBuildCheck.log` or
`Retro68:SherclawkNativeBuildErrorCheck.log`. The good diagnostic stages exact
current starter sources into a fresh `Retro68:NativeBuildCheck<ticks>:` folder,
then sends separate queued MrC, PPCLink and Rez commands through `src/native/toolserver.c`.
It verifies both forks, the PowerPC PEF header, `cfrg`, `SIZE`, Finder `APPL/SHTP`
and the closed success record before native launch. Expected title/text in the
launched window supply visual acceptance beyond the recorded process observation.
The error variant expects a deliberate MrC failure and verifies there is no
application, success record or launch. Each attempt retains its own inputs.

`S` or Command-period stops observation and subsequent steps; an outstanding
reply is drained without advancing. A 120-second command deadline or observed
ToolServer disappearance likewise records an unknown outcome. Command-Q leaves
the diagnostic. Never launch a partial artifact from a stopped attempt.
The fixture logs every running process as evidence of the executor
environment; this is not queue locking.

Both success runs, compiler failure and Stop/late-reply behavior passed in OS 9.
The repeated artifact payloads matched except for a PEF timestamp; full resource
maps also differed, so complete fork determinism is not claimed. Detailed
fixtures and timings are in [native executor evidence](history/native-executor-evidence.md).
This fixed diagnostic remains separate from the [native executor](native-builds.md#native-executor).

## Timing and transport experiments

`Retro68:Sherclawk.log` also records timing, with no request or response text.
Each model round (and the handoff request) ends with one line of tick offsets
since the request began, `-` for a phase never reached: `round=3 init=2
connect=10 handshake=100 sent=104 first_byte=500 done=620 close=740 up=4096
down=812 out=240/6000 reasoning=180 end=ok`. `close` includes the Open Transport
teardown yields on a completed round (an aborted round logs the abort instant;
its drain and teardown follow), `up` and `down` are request and response bytes,
`out` is the provider-reported completion tokens against the request cap and
`reasoning` the part of them spent reasoning (each omitted when the provider
does not report it), `end=abort` marks a round that stopped early and
`end=truncated` one cut off at the output limit and retried; `end=discarded` is
a complete reply dropped for an oversized or excess tool call and retried. Each
tool call logs `tool=<n> name=<tool> ticks=<elapsed>`; for `build_project` and
`run_application` that spans the whole stepped operation.

The Open Transport teardown that dominates `close` is tunable at build time for
guest soaks: `SHERCLAWK_OT_YIELD_TICKS` (default 10; it was 60) sets the yield on each side
of `CloseOpenTransport`, and `SHERCLAWK_OT_KEEP_OPEN_AFTER_CLEAN=1` leaves OT
open after a cleanly completed round (errors, aborts and quit still cycle it).
Set either in `config.local.h`; the defaults keep the long-standing policy.

Abort-mid-connect regression path: with `Sherclawk` running in the guest, send
with Command-Return and issue Command-Period a fraction of a second later — the
tightest reproducible stop is two QMP `send-key` chords about 15 ms apart. The
abort line should carry `end=abort` with no `sent` tick (`connect=-` when the
stop landed before the first TCP connect mark), the app must stay running, and
a later request must still complete. Save Handoff and Command-Q during a
request take the same drain. It applies only while the connect or handshake
is pending and never sends the request; Command-Q hides the window while it
waits. The recorded guest outcome is in the
[verification history](history/verification.md#abort-mid-connect-drain-october-10-2026).

### Owned application Quit diagnostic

Build `./build.sh SherclawkQuitCheck_APPL`, publish with
`SHARE_HOST=<afp-host> APP=SherclawkQuitCheck tools/deploy-to-share.sh`, and launch
`Retro68:SherclawkQuitCheck` in OS 9.2.2. Read `Retro68:SherclawkQuitCheck.log`
and `Retro68:SherclawkQuitIgnore.log` on the share. Expected: one Quit received
by the responsive independent app (which accepts but ignores it),
`uncertain/QUIT_TIMEOUT`, a subsequent launch with `PROCESS_PREEXISTED` and its
original close handle, `error/RUN_NOT_OWNED` for the new handle, then
`ok/QUIT_OBSERVED` for the generated starter and `RESULT failures=0`.
The diagnostic window should remain responsive while it observes. The ignoring
app remains running; close it manually with Command-Q after reviewing evidence.
Run in a clean diagnostic session or account for prior ignore-log lines. Record
actual guest evidence in `docs/history/verification.md` before advertising the
tool in both model schemas and environment discovery.

### Large text acceptance

`./build.sh SherclawkLargeTextCheck_APPL` builds the full app with the
experimental 64 KiB file/input cap; shipping `Sherclawk_APPL` remains gated at
4 KiB. `SherclawkEditCheck` writes 16/64 KiB native fixtures and logs step/phase
ticks, heap headroom, exact backups/replacements and fresh revisions.
`SherclawkBuildCheck` repairs an exactly 64 KiB input and builds a new immutable
snapshot with ToolServer. Set the diagnostic's partition to the 10 MiB minimum
before measuring. Preserve logs, journals and backups. See
[the complete acceptance procedure](large-text.md); do not set
`SHERCLAWK_LARGE_TEXT_VERIFIED` until guest evidence is recorded.
