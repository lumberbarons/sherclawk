# Building, deploying and checking Sherclawk

Run the commands in this guide from the repository root. Host checks use
Toolbox stubs; a successful host run does not establish guest behavior.
[AGENTS.md](../AGENTS.md) contains contributor conventions and the source map.

## Build and publish

Prerequisites: Docker with the `ghcr.io/autc04/retro68` image, host Python 3
(stdlib only) for `build.sh` and the artwork converter, and `ssh`/`scp` for
publishing, plus a Certainly clone next to this repository, your own lawfully
obtained Apple Universal Interfaces 3.4 SDK, and
the vendored `patches/` + `art/` in this tree. Publishing needs an AFP
share host reachable over `ssh`/`scp` with passwordless `sudo -n`, a `macos9`
user and group, python3 and netatalk; files go to `/srv/retro68` unless
`SHARE_DIR` overrides it. The converter reads
`art/sherclawk.png` (8-bit RGBA, noninterlaced); override its path with
`SHERCLAWK_ART`.

```bash
git clone --recursive --depth 1 https://github.com/minorbug/certainly.git ../Certainly
# Supply your own SDK; this is the Interfaces&Libraries folder, not its parent.
export INTERFACES_DIR="/path/to/Interfaces&Libraries"
# Optional compiled fallbacks; normal runtime keys can be entered in Preferences.
cp config.example.h config.local.h
# Set SHERCLAWK_API_KEY and optionally SHERCLAWK_MODEL / SHERCLAWK_WORKSPACE.
./build.sh
SHARE_HOST=<afp-server> tools/deploy-to-share.sh
```

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
```

Set `CERTAINLY_DIR` if the clone is elsewhere. Without Certainly,
`tools/check.sh` skips its network suite and `tools/check-transport.sh` fails.
The host checks compile shared code with `cc` and regenerate
`build/project-template.h`. The lint gate uses shellcheck, cppcheck and Ruff;
CI installs all three. Full builds also regenerate `build/art.r`, so use
`build.sh` rather than configuring CMake by hand in a fresh tree.

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
| `SherclawkWriteCheck`, `SherclawkEditCheck`, `SherclawkSearchCheck`, `SherclawkInspectCheck` | The matching tool executors against the File Manager |
| `SherclawkViewImageCheck` | `view_image` against the File Manager: a PNG at the size cap read in bounded steps, refusals and the vision gate |
| `SherclawkProjectCheck` | `create_project` publication, bytes and metadata |
| `SherclawkBuildCheck`, `SherclawkRunCheck`, `SherclawkSelfBuildCheck`, `SherclawkSelfBuildStopCheck` | `build_project` / `run_application` through the native executor (`tools/build-check.c`) |
| `SherclawkToolServerCheck`, `SherclawkNativeBuildCheck`, `SherclawkNativeBuildErrorCheck` | ToolServer channel and fixed native build, described below |
| `SherclawkHandoffCheck` | Save Handoff persistence and seeding |
| `SherclawkScrollCheck` | Toolbox scrollbar behavior |

Host-side tests are in `tests/`, compiled against the stubs in `tests/toolbox/`
by `tools/check.sh`; `tools/lint.sh` adds shellcheck, cppcheck and ruff for the
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
```

The cursor matcher can mistake highlights in the lobster artwork for the arrow.
After using UTM, check `info mice`; this VM needs `mouse_set 4` for the
working relative HID mouse. The active absolute tablet ignores relative input.
For UI diagnostics, reanchor, use blind `moveto`, verify a screenshot, then
use `tools/guest-input.py --click` with the pointer still.

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
then sends separate queued MrC, PPCLink and Rez commands through `toolserver.c`.
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
teardown yields, `up` and `down` are request and response bytes, `out` is the
provider-reported completion tokens against the request cap and `reasoning` the
part of them spent reasoning (each omitted when the provider does not report it),
`end=abort` marks a round that stopped early and `end=truncated` one cut off at
the output limit and retried; `end=discarded` is a complete reply dropped for an
oversized or excess tool call and retried. Each tool call logs `tool=<n> name=<tool> ticks=<elapsed>`; for
`build_project` and `run_application` that spans the whole stepped operation.

The Open Transport teardown that dominates `close` is tunable at build time for
guest soaks: `SHERCLAWK_OT_YIELD_TICKS` (default 10; it was 60) sets the yield on each side
of `CloseOpenTransport`, and `SHERCLAWK_OT_KEEP_OPEN_AFTER_CLEAN=1` leaves OT
open after a cleanly completed round (errors, aborts and quit still cycle it).
Set either in `config.local.h`; the defaults keep the long-standing policy.

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
