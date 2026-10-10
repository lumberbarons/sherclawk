# Sherclawk — The consulting crustacean

A native coding agent for classic Mac OS 9, written in C99 and cross-compiled
with Retro68. It connects directly to OpenRouter over HTTPS and performs tool
work inside the Mac application. The mascot is a lobster; the name combines
Sherlock and claw.

Sherclawk can inspect its workspace, search and edit source files, create
PowerPC Toolbox projects, compile them with the guest's MrC/PPCLink/Rez tools,
and launch verified build artifacts. It uses native File Manager, Toolbox and
Process Manager APIs, with a cooperative event loop and Stop controls.

## Current capabilities and limits

- Sequential model/tool/result conversations with saved session journals and
  Markdown handoffs.
- Workspace catalog and text reads, literal search, create-only writes and
  folders, revision-guarded edits, and read-only platform inspection.
- Optional starter projects or independently assembled project descriptors,
  native MPW ToolServer builds, and guarded app launch.
- Preferences for the model, API key, workspace and per-run limits.

This is experimental software with explicit bounds: source writes and edits
are limited to 4 KiB, paths are classic colon-separated workspace paths, and
text files use MacRoman with CR line endings. Journals preserve conversations
but are not automatically reloaded after quitting. Stop prevents further work;
it does not undo completed operations or prove cancellation of a published
build. Launch observation does not establish functional or visual correctness.

Remote MCP has a protocol core and a native diagnostic, but the main app does
not expose MCP tools or a configuration editor yet. Reasoning-effort metadata
is displayed in Preferences; effort is not yet saved or sent. Follow active
work in [GitHub issues](https://github.com/lumberbarons/sherclawk/issues).

## Prerequisites

To cross-compile the app:

- Docker with `ghcr.io/autc04/retro68` and host Python 3; Python helpers use only
  the standard library.
- A separate [Certainly](https://github.com/minorbug/certainly) clone.
- Your own lawfully obtained Apple Universal Interfaces 3.4 SDK. The repository
  neither provides nor downloads it. Supply the complete `Interfaces&Libraries`
  folder, including Open Transport headers and PPC libraries; see
  [SDK setup](docs/development.md#sdk-setup) for the exact layout.

To run it, use a classic Mac OS 9 system or your own configured guest, a mounted
workspace, network access and an OpenRouter API key. Native project builds also
need the guest's ToolServer and MrC/PPCLink/Rez installation. The
[template guide](templates/ppc-toolbox/README.md) records tested toolchain versions.

## Build and run

Run from the repository root:

```bash
git clone --recursive --depth 1 https://github.com/minorbug/certainly.git ../Certainly
export INTERFACES_DIR="/path/to/Interfaces&Libraries"
./build.sh
```

The default target is `Sherclawk_APPL`; outputs are in `build/`. Transfer and
decode `Sherclawk.bin` with a MacBinary-aware utility in OS 9, or use the
[AFP deployment workflow](docs/development.md#deployment). A raw `.APPL` data
fork alone is not sufficient.

For an existing AFP share host configured for deployment:

```bash
export SHARE_HOST="<afp-host>"
tools/deploy-to-share.sh
```

Quit a running copy before republishing. Launch Sherclawk in OS 9, open
**Edit > Preferences**, enter your key and select a model, then set an existing
workspace such as `Retro68:`. Send a message with **Send** or Command-Return;
use **Stop** or Command-Period to stop further execution.

Credentials are not needed to build or launch the UI. Optional compiled
fallbacks belong in ignored `config.local.h`; omit that file for distributable
builds and let users enter their own key. Preferences stores the key as clear
text, so protect the guest's preferences and disk images. Session journals can
contain conversation and workspace file contents. See [usage](docs/usage.md).

## Development checks

Host checks need no Docker, guest or API key; transport checks require Certainly:

```bash
tools/check.sh
tools/check-transport.sh
tools/lint.sh
```

The [development guide](docs/development.md) covers Docker builds, deployment,
probe configuration, diagnostic launch/read commands and timing logs.
Host success alone is not guest acceptance.

## Documentation

| Guide | Purpose |
|---|---|
| [Usage](docs/usage.md) | Preferences, controls, sessions, handoffs and Stop |
| [Tools](docs/tools.md) | File, search, edit and inspection contracts |
| [Native builds](docs/native-builds.md) | Descriptors, snapshots, executors and launch authority |
| [Development](docs/development.md) | Build, deploy, check and diagnose the application |
| [Architecture](docs/architecture.md) | Native execution and persistence principles |
| [Limits](docs/limits.md) | Buffer, token, memory and deadline relationships |
| [MCP](docs/mcp.md) | Protocol diagnostic and remaining integration gates |
| [PowerPC template](templates/ppc-toolbox/README.md) | Starter UI, native compilation, runtime logging and self-render |
| [OS 9 guest setup](docs/macos9-qemu.md) | Optional QEMU/UTM testbed notes |
| [Verification history](docs/history/verification.md) | Dated acceptance evidence |
| [Archived design notes](docs/archive/README.md) | Historical proposals and research |

Contributor instructions and source layout are in [AGENTS.md](AGENTS.md).

## License

MIT, see [LICENSE](LICENSE). Adapted and separately supplied third-party code,
runtime dependencies and binary-distribution obligations are described in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

The lobster artwork in `art/sherclawk.png` was generated with ChatGPT and is
offered under the same MIT license to the extent the project holds rights in it.
Macintosh, Mac OS and iMac are trademarks of Apple Inc. Sherclawk is an independent
project and is not affiliated with or endorsed by Apple Inc.

`read_text` fills pages against the 1536-byte JSON result budget after UTF-8
conversion and escaping, within the requested line limit (default 20, maximum
30). Continue with `next_byte` when a line is partial, or `next_line` when
available. CRLF pairs stay together; revisions are independent of pagination.
