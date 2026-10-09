# Guest verification — October 3, 2026

These results describe the original two-line, content-click-to-quit starter.
The native Clear button, TextEdit field and bounded runtime logger added in
issue #87 have not yet been verified with MrC or in the OS 9 guest. The current
interaction/log acceptance steps are in README.md; host and Retro68 checks
do not establish guest behavior or MrC compatibility.

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
