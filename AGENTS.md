# AGENTS.md

Sherclawk is a native agent-loop app for classic Mac OS 9 (PowerPC), written in
C99 and cross-compiled with Retro68. It is not a Unix or modern-macOS program:
the app calls Toolbox APIs, and only a subset of sources compiles on the host,
against the stub headers in `tests/toolbox/`.

## Fast checks (host, no Docker/VM/API key)

```bash
tools/check.sh                # ASan/UBSan protocol, loop and File Manager fault suites
tools/check-transport.sh      # TLS I/O against patched Certainly sources
python3 tests/test_worker.py  # MacRelix worker protocol (host perl)
```

- `tools/check.sh` is the main loop while editing; run it before claiming a
  change works. It regenerates `build/project-template.h` and compiles the
  shared sources with plain `cc`.
- There is no test runner: each suite is a standalone binary under
  `build/tests/`. To rerun one suite, reuse its `cc` line from `tools/check.sh`.
- Certainly is fetch-only and never committed:
  `git clone --recursive --depth 1 https://github.com/minorbug/certainly.git ../Certainly`
  (override with `CERTAINLY_DIR`). Without it, check.sh skips the network suite
  and check-transport.sh exits with an error.

## Full app build (Docker)

```bash
tools/get-universal-interfaces.sh   # one-time: fetch Apple's Universal Interfaces
cp config.example.h config.local.h  # optional local config; gitignored
./build.sh [SherclawkFoo_APPL]      # default target Sherclawk_APPL; output in build/
```

- `build.sh` runs the `ghcr.io/autc04/retro68` image, swaps in the Universal
  Interfaces, stages Certainly and applies `patches/*.patch` to the staged copy
  (the upstream clone stays pristine), then builds with CMake.
- The first argument / `BUILD_TARGET` selects a CMake diagnostic target, e.g.
  `SherclawkProbe_APPL` or `SherclawkWriteCheck_APPL` (see `CMakeLists.txt`).
- Do not configure CMake by hand in a fresh tree: build.sh and check.sh
  generate `build/art.r` and `build/project-template.h` first; `tools.c`, some
  diagnostics and `tests/test_tools.c` include the generated header directly.

## Deploying to the guest

- `SHARE_HOST=<afp-host> [APP=SherclawkFooCheck] tools/deploy-to-share.sh`
  builds the app, adds netatalk fork metadata and publishes it via ssh/scp to
  the AFP share (`/srv/retro68`; override `SHARE_DIR`). `SHARE_HOST` has no
  default. Quit a running copy before republishing and never restart AFP/
  netatalk during a guest session.
- Verification beyond host tests means the OS 9 guest: launch the diagnostic
  and read `Retro68:<Name>.log`. Per-diagnostic launch/read commands are in
  README.md under "Files and verification".

## Layout

- `main.c` Toolbox UI/event loop; `agent.c` provider loop; `tools.c` +
  `inspect.c` tool executors (shared with host tests via `TOOLS_SRC` in
  CMakeLists.txt); `chat.c`/`json.c`/`text.c`/`network.c`/`vendor/http.c` core.
- Native build/run machinery: `build_project.c`, `selfbuild.c`, `toolserver.c`,
  `run_application.c`, `jobs.c`; `worker/` is the Perl fallback executor for
  jobs in the guest.
- `tests/` uses `tests/toolbox/` stubs to compile app sources with host `cc`;
  `tools/*-check.c` are Mac GUI diagnostics built by CMake, not host programs.
- `patches/` are fixes applied to staged copies of Certainly by build.sh and
  check-transport.sh. `vendor/` is imported code (HelloChat HTTP plus the host
  TLS shim); `SHERCLAWK_APP` gates Sherclawk's own User-Agent there.
- Generated files and secrets stay in `build/` and `config.local.h` (both
  gitignored). Never commit credentials; builds without them still launch.

## Conventions

- C99, `-Wall -Wextra` (`-Werror` in host tests). Sources shared between the
  app and host tests must keep compiling under both toolchains.
- Runtime work is cooperative and bounded: builds, polling and search step
  from the event loop in small increments and honor Stop. Do not add blocking
  loops or unbounded waits.
- Model-facing contracts (8 KiB JSON argument/result bounds, MacRoman/CR text
  and colon paths relative to the `Retro68:` workspace, create-only writes,
  revision-guarded edits) are documented in README.md; update the matching
  section when tool behavior changes.
- The Python tools are stdlib-only; keep them that way.
