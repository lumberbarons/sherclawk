# Guest verification — October 3, 2026

These results describe the original two-line, content-click-to-quit starter.
The native Clear button, TextEdit field and bounded runtime logger added in
issue #87 have not yet been verified with MrC or in the OS 9 guest. The current
interaction/log acceptance steps are in README.md; host and Retro68 checks
do not establish guest behavior or MrC compatibility. The same applies to the
self-render files added in issue #82; see the section at the end of this file.

| Component | Recorded identity |
|---|---|
| Guest | Mac OS 9.2.2, PowerPC G3 under QEMU/UTM |
| MacRelix | Experimental snapshot; vers 1 `2026-08-09 17:57`; build date `Sun Aug 9 17:58:32 EDT 2026`; platform `os9` |
| MrC | Banner `MrC C Compiler 4.1.0f1c1` |
| PPCLink | vers 1 `1.5.2` |
| Rez | vers 1 `3.4.1` |
| ToolServer | vers 1 `3.5` |
| SDK | Universal Interfaces 3.4; `UNIVERSAL_INTERFACES_VERSION 0x0340` |

Native `GetProcessInformation` confirmed these running application paths:

```text
Macintosh HD:Applications (Mac OS 9):MacRelix:MacRelix
Macintosh HD:Applications (Mac OS 9):MPW-GM:MPW:ToolServer
```

ToolServer's MPW directory was
`Macintosh HD:Applications (Mac OS 9):MPW-GM:MPW:`. CIncludes, RIncludes,
SharedLibraries and PPCLibraries resolved into the sibling
`Interfaces&Libraries` tree (echoed `MPW::` is classic parent traversal).
DeRez read linker, Rez and ToolServer version resources at that installation.
The SDK version came from its ConditionalMacros.h.

MacRelix binary fingerprints captured from the running application's forks,
SHA-256:

```text
data (679336 bytes): 236f5ec235aee4337fdba6a3f2c81eda2024f561fe96ae2778a520844ef84c1a
resource: a604fd6c173663bcfc209d8a5411df35e6df5e62414e59cf64435509f8acb48b
```

The corresponding upstream source revision remains unknown. These identify
the installed binary; no source-revision equivalence is claimed. Third-party
binaries and SDK headers are not tracked.

Native `good03`, `good04` and `good05` builds in `Retro68:Template01:`
completed compile, link, resource append and metadata stages with exit 0.
`success.txt` named `Template`. MacRelix `open` launched `good04:Template`;
a guest screenshot showed the expected title and both text lines. Command-Q
quit it. This is visual launch verification, not a runtime test protocol.

`bad04` and `bad05` used the deliberate error source and returned exit 1:

```text
stage=prepare started
stage=compile started
File "main.c"; line 1 #Error: Sherclawk deliberate compiler failure
```

Failed directories contained copied source/resource files and no executable
or success record. Restoring source and building `good05` succeeded. Reusing
`good03` returned exit 1 (`File exists`) without altering its artifact.
`process06` built and launched the native Process Manager diagnostic and
recorded the actual executor paths above.

The final recipe was also published under fresh filenames to avoid AFP
source caching: `good08` completed with exit 0; `../escape` was refused with
exit 1 before directory creation. `toolchain8.log` captures the final version
probe and MacRelix build date. Its version resources were decoded separately
from the captured resource fork.

Evidence remains in the guest fixture: build directories, `bad05.log`,
`good05.log`, `good08.log`, status files, `toolchain8.log` and `runtime.log`. Local evidence
is under ignored `build/native-template/`; screenshots and binary
forks are ignored. No AFP restart, disk-image change or host compilation was
needed for these native applications.

## Self-render (issue #82) — host and Retro68 only, not yet run in the guest

Checked on the host and with Retro68 (GCC), which does not establish MrC or
Mac OS 9 behavior:

- `tests/template/test_template.c` runs `io.c`, `png.c` and `selfrender.c`
  against a modelled Toolbox under ASan/UBSan: the PNG is parsed by an
  independent decoder (chunk CRCs, stored-block lengths, Adler-32, palette,
  every pixel, widths around the 8-bit length boundary), and the request/marker
  protocol, frame spacing, clamping, stale-marker removal and error reporting
  are asserted. The same bytes also decompress with Python's `zlib`.
- `./build.sh SherclawkTemplate_APPL` compiles and links all five sources with
  `-Wall -Wextra` and no warnings; `tools/native-build-check.c` and
  `build-native.sh` were updated for the same sources but not run.

Linked starter size from `powerpc-apple-macos-size` (Retro68, not MrC): text
73,728 + data 1,536 + bss 1,152 bytes. The heap cost of a frame is
`width * height` plus row padding for the `GWorld` and `width + 6` bytes for the
PNG row, computed, not measured.

Still to run on OS 9.2.2, then record here with the build ID and the log:

1. The five sources compile under MrC with `-i ":"`, in the `build_project`
   path and in the fixed `build-native.sh`/`native-build-check` recipes, and
   `SIZE` reads 2 MiB / 1.5 MiB.
2. Launch through `run_application` (`launchDontSwitch`), leave Sherclawk in
   front, and read `frame.ready`: `status=ok`. Confirm `frame1.png` shows the
   field frame, the Clear button and any typed text, and that the window was
   never fronted. This is the occluded-window claim; it has not been observed.
3. Offscreen drawing of the push button (swapped `contrlOwner`, including its
   active/inactive look while the app is backgrounded) and of TextEdit (swapped
   `inPort`, no flicker in the real window) behave as intended; the window
   after the frame is unchanged.
4. A `frame.req` containing `overlay selftest frames=3` is picked up while the
   app is in the background within about a second, produces three frames, and
   the image reads `selftest PASS`. Break the hit test (swap `h` and `v` in
   `SetRect` for the button) and confirm the overlay and `selftest FAIL` show it.
5. A window far larger than the heap reports `status=error code=-108` and the
   app keeps running; record the largest content size that renders at the
   minimum partition.
6. Existing behavior still holds: updates, Clear, drag, close box, Command-Q and
   the `Logging unavailable` title with a read-only folder.
