# AGENTS.md

Sherclawk is a native agent-loop app for classic Mac OS 9 (PowerPC), written in
C99 and cross-compiled with Retro68. It is not a Unix or modern-macOS program:
the app calls Toolbox APIs, and only a subset of sources compiles on the host,
against the stub headers in `tests/toolbox/`.

## Fast checks (host, no Docker/VM/API key)

```bash
tools/check.sh                # ASan/UBSan protocol, loop and File Manager fault suites
tools/check-transport.sh      # TLS I/O against the Certainly sources
tools/lint.sh                 # shellcheck, cppcheck and ruff (the CI lint gate)
```

- `.github/workflows/ci.yml` runs `tools/check.sh`, `tools/check-transport.sh`
  and `tools/lint.sh` on `ubuntu-24.04` for pushes to main and every pull
  request; `tools/check.sh` therefore has to stay clean
  under GCC as well as clang.

- `tools/check.sh` is the main loop while editing; run it before claiming a
  change works. It regenerates `build/project-template.h` and compiles the
  shared sources with plain `cc`.
- There is no test runner: each suite is a standalone binary under
  `build/tests/`. To rerun one suite, reuse its `cc` line from `tools/check.sh`.
- Certainly is fetch-only and never committed. Sherclawk builds against the
  `lumberbarons/certainly` fork, not upstream `minorbug/certainly`, at the
  commit pinned in `tools/certainly.sh`:
  `git clone --depth 1 https://github.com/lumberbarons/certainly.git ../Certainly`
  (override with `CERTAINLY_DIR`). Scripts check a clean clone out to the pin
  and CI fetches exactly it; bump the pin only in a PR with
  `tools/certainly-bump.sh`. Without the clone, check.sh skips the network
  suite and check-transport.sh exits with an error.

## Full app build (Docker)

```bash
# Supply your own lawfully obtained Apple Universal Interfaces 3.4 SDK.
export INTERFACES_DIR="/path/to/Interfaces&Libraries"
cp config.example.h config.local.h  # optional local config; gitignored
./build.sh [SherclawkFoo_APPL]      # default target Sherclawk_APPL; output in build/
```

- All code changes must pass a Docker-based `./build.sh` before a PR is created:
  the host checks do not compile `src/app/main.c` or the other Toolbox-only sources.
- `build.sh` runs the `ghcr.io/autc04/retro68` image, swaps in the Universal
  Interfaces, stages a copy of Certainly, then builds with CMake. Certainly fixes belong in
  the fork, not in this repo.
- The SDK is user-supplied, never downloaded or redistributed by this repo.
  `INTERFACES_DIR` points to the complete `Interfaces&Libraries` folder;
  the default is `../InterfacesAndLibraries`. See
  [SDK setup](docs/development.md#sdk-setup) for its layout.
- The first argument / `BUILD_TARGET` selects a CMake diagnostic target, e.g.
  `SherclawkProbe_APPL` or `SherclawkWriteCheck_APPL` (see `CMakeLists.txt`).
- Do not configure CMake by hand in a fresh tree: build.sh and check.sh
  generate `build/art.r` and `build/project-template.h` first; `src/tools/tools.c`, some
  diagnostics and `tests/test_tools.c` include the generated header directly.

## Deploying to the guest

- `SHARE_HOST=<afp-host> [APP=SherclawkFooCheck] tools/deploy-to-share.sh`
  builds the app, adds netatalk fork metadata and publishes it via ssh/scp to
  the AFP share (`/srv/retro68`; override `SHARE_DIR`). `SHARE_HOST` has no
  default. Quit a running copy before republishing and never restart AFP/
  netatalk during a guest session.
- If a freshly published app fails to launch with "could not be opened,
  because it is in use", suspect a stale server-side AFP session before the
  guest: an unclean guest disconnect (host sleep, VM reset) can leave `afpd`
  processes with no live client connection holding the old app and its `._`
  sidecar open on the share host. Confirm with `ss`/`lsof` that the process
  has no client and holds the app path, kill only those stale sessions, and
  leave netatalk and the live mount alone.
- Verification beyond host tests means the OS 9 guest: launch the diagnostic
  and read `Retro68:<Name>.log`. Per-diagnostic launch/read commands are in
  [Files and verification](docs/development.md#files-and-verification).

## Layout

- Sources live under `src/`: `app/` (Toolbox UI, transcript, display math,
  session journal, preferences, timing), `agent/` (provider loop, tool registry,
  network, JSON and text core), `tools/` (model-facing tool executors),
  `mcp/` (remote MCP client, run controller, config editor) and `native/`
  (ToolServer builds, application launch/Quit, jobs). `vendor/` and `config.h` stay
  outside it. Includes are written relative to `src/` (`#include
  "agent/json.h"`); builds pass `-I . -I src -I vendor`, so a new directory or
  file needs no include-path change but does need adding to `CMakeLists.txt`
  and the `tools/check.sh` / `tools/lint.sh` source lists.
- `src/app/main.c` Toolbox UI/event loop and `RunHost` effects; `src/app/run_ctl.c`
  owns send/tool/handoff transitions, limits, Stop and transport drains in
  `RunCtl` (host-testable with a fake network); `src/app/display.c` pure
  formatting, validation and scroll/layout math; `src/app/chat.c` bounded transcript
  capping and tool-line folding; `src/app/prefs_model.c` the Preferences model
  picker (rows, filter, fetch transitions, confirm) behind a `PrefsModelHost`
  callback struct (host-testable); `src/app/session.c` journal and
  handoff persistence (host-testable); `src/agent/agent.c` provider loop; `src/tools/tools.c` +
  `src/tools/inspect.c` tool executors (shared with host tests via `TOOLS_SRC` in
  CMakeLists.txt); `src/tools/fsutil.c` shared catalog identity, revisions and the verified staged-body write (create,
  write, flush, catalog proof, read-back) the publishing callers share;
  `src/agent/jsonargs.c` shared bounded argument validation and JSON output buffers;
  `src/agent/registry.c` the one table defining every tool's name, wire
  schema and `mutates`/`pending` classification (pure data so host suites that
  link only `src/agent/agent.c` can join the schemas without the executors); `src/app/chat.c`/`src/agent/json.c`/`src/agent/text.c`/`src/agent/network.c`/`vendor/http.c` core.
- Remote MCP in the run loop: `src/mcp/run.c` is the controller (load, discover,
  freeze, call, retain, Stop, drain) behind a host callback struct;
  `src/mcp/client.c`, `src/mcp/protocol.c`, `src/mcp/stream.c` and `src/mcp/config.c` are the
  client, `src/mcp/result.c` retains and pages results, `src/mcp/editor.c` and
  `src/mcp/store.c` back the configuration editor. See `docs/mcp.md`.
- Native build/run machinery: `src/native/build_project.c`, `src/native/selfbuild.c`, `src/native/toolserver.c`,
  `src/native/run_application.c`, `src/native/jobs.c`. MPW ToolServer is the only build backend; the
  queue's `worker-lock` and `STOP` names are kept for existing queues.
- `tests/` uses `tests/toolbox/` stubs to compile app sources with host `cc`;
  most `tools/*-check.c` are Mac GUI diagnostics built by CMake, not host
  programs. `tools/native-process-check.c` is the exception: a guest source
  fixture built as `main.c` of a starter project (see
  `templates/ppc-toolbox/README.md`).
- `vendor/` is imported code (HelloChat HTTP plus the host
  TLS shim); `SHERCLAWK_APP` gates Sherclawk's own User-Agent there.
- Generated files and secrets stay in `build/` and `config.local.h` (both
  gitignored). Never commit credentials; builds without them still launch.

## Conventions

- C99, `-Wall -Wextra` (`-Werror` in host tests). Sources shared between the
  app and host tests must keep compiling under both toolchains.
- Runtime work is cooperative and bounded: builds, polling and search step
  from the event loop in small increments and honor Stop. Do not add blocking
  loops or unbounded waits.
- Buffer, token and deadline limits depend on each other; read
  `docs/limits.md` before changing any of them (`src/agent/agent.h`, `src/app/chat.h`, `src/agent/network.h`,
  the `SIZE` resource) and keep it current.
- Model-facing contracts (fixed JSON argument/result bounds, MacRoman/CR text
  and colon paths relative to the `Retro68:` workspace, create-only writes,
  revision-guarded edits) are documented in [docs/tools.md](docs/tools.md) and
  [docs/native-builds.md](docs/native-builds.md); update the matching section
  when tool behavior changes. Runtime usage is in [docs/usage.md](docs/usage.md),
  and lasting design principles are in [docs/architecture.md](docs/architecture.md).
- The Python tools are stdlib-only; keep them that way.

## Architectural Decisions

ADRs live in `docs/adr/`. Read the relevant ADRs before proposing architectural changes — they encode constraints and rejected alternatives. When writing or modifying a spec, cite the ADRs that constrained it in the spec's own frontmatter; ADRs do not track their downstream consumers.

| ADR | When this applies |
|---|---|
| `docs/adr/0001-execute-builds-natively-through-toolserver.md` | Any change to how builds execute: `src/native/selfbuild.c`, `src/native/toolserver.c`, the build queue, `worker-lock` ownership, retry/replay of builds, or reintroducing MacRelix or a shell worker |
| `docs/adr/0002-run-tests-in-separate-native-app.md` | Designing `test_project`, a test target in the project descriptor, or any place project-authored code would run |
| `docs/adr/0004-load-agents-md-as-bounded-owner-guidance.md` | Historical instruction-loading policy, superseded by ADR-0008; read for the original constraints and rejected alternatives |
| `docs/adr/0008-load-folder-guidance-at-startup-and-on-demand.md` | Changing workspace or nested-folder `AGENTS.md` discovery, scoped delivery, caching, handoff restoration, bounds or authority framing |
| `docs/adr/0003-admit-tools-read-only-first.md` | Adding or changing a model-facing tool, especially one that mutates files, Finder metadata, resources or other processes |
| `docs/adr/0005-expose-bounded-read-only-self-diagnostics.md` | Adding self-diagnostic state/tools, retaining lifecycle events across launches, exporting diagnostic reports, or changing diagnostic privacy and retention |
| `docs/adr/0006-use-explicit-local-skills-with-in-app-authoring.md` | Designing skill discovery, explicit invocation, conversation context or handoff retention, package validation, or bundled and in-app skill authoring |
| `docs/adr/0007-keep-data-separate-from-application.md` | Changing installation, first-launch setup, data-root or workspace selection, session/MCP record locations, or migration of existing workspace data |

## Owned application Quit

- `src/native/application_process.c` owns only processes newly launched and journaled by
  this Sherclawk process. Do not reset ownership on New Chat, restore it from
  journals, force quit, or automatically clean up applications.
- `src/native/ae_dispatch.c` owns the shared answer handler and lifetime-unique return IDs
  for ToolServer and Quit. Match both ID and sender PSN; never reuse IDs.
- `quit_application` is advertised after the OS 9.2.2 `SherclawkQuitCheck` pass
  recorded in `docs/history/verification.md`. Preserve that guest gate for new
  process tools. See `docs/quit-application.md`.
