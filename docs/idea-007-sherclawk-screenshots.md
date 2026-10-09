# 007 — Sherclawk screenshots: native capture, and seeing what it built

**Status:** idea, not built. API availability was checked against the workspace's
locally installed, user-supplied Universal Interfaces (`InterfacesAndLibraries/Interfaces/CIncludes/`);
nothing here has been compiled or run in the guest. OS 9's built-in screenshot
behavior is documented from vintage sources, not from this workspace's 9.2.2
install, and whether the target's QuickTime provides a JPEG graphics exporter
still needs a guest check (October 2026).

## The idea

Two capabilities, worth separating because only the second is blocked today:

1. **`capture_screen` as an artifact tool.** Sherclawk copies the framebuffer
   with QuickDraw, encodes it (PICT natively, JPEG through QuickTime), and saves
   a file in the workspace. No model change is needed for this to be useful:
   it closes `run_application`'s visual loop, feeds evidence into session
   records, and gives the user a picture. An automatic capture after
   `run_application` would replace the current explicit
   `smoke_test: not_performed` with "process present **and** something drew".
2. **A picture the model can see.** The text protocol cannot carry an image
   today; `docs/idea-004-sherclawk-os9-tools.md` §10 already named this the
   reason a screenshot tool was deliberately left out of the OS 9 tools
   list. Extending the next request with an OpenRouter-compatible `image_url`
   part lets a vision model judge layout and appearance — which is what "make
   it look better" actually needs.

Everything here is guest-side. The emulator QMP helpers are irrelevant once
Sherclawk runs on the real iMac; QuickDraw reads the built-in display exactly
the same way, and OS 9 has no screen-capture permission model.

## 1. What OS 9 gives natively

| Input | Result |
|---|---|
| ⌘⇧3 | Full screen → PICT `Picture N` at the root of the startup disk |
| ⌘⇧4 | Drag a region; on OS 9, hold Control while selecting → Clipboard |
| ⌘⇧Caps Lock-4 | Click a window for a window shot (classic only; Ctrl variant → Clipboard) |
| ⌘⌃⇧3 / ⌘⌃⇧4 | Full screen / region → Clipboard |
| **Grab** (ships with the OS) | Selection / Window / Screen / **Timed Screen** (10 s), saves PICT |

All of these are interactive: Grab is not scriptable, there is no AppleScript
screenshot verb, and the `screencapture` CLI is OS X. So for the agent the
native route is not keystroke injection — it is Sherclawk doing the capture
itself with the same QuickDraw primitives the system shortcuts use internally.

## 2. In-app capture is a QuickDraw job

The whole capture is small and already half-written in this codebase
(`main.c:454` uses `CopyBits` for the artwork):

1. `GetMainDevice()` → `(**gd).gdPMap` for the menu-bar screen; iterate
   `GetDeviceList` if secondary displays ever matter (the iMac has one).
2. `NewGWorld()` a buffer at screen depth (8- or 16-bit is enough), then
   `LockPixels`/`GetGWorldPixMap`.
3. `CopyBits` screen PixMap → GWorld PixMap: whole screen or any sub-rect.
4. Encode:
   - **PICT** via `OpenPicture`/`ClosePicture`, written to a data fork with
     Finder type `PICT` — zero dependencies, guaranteed.
   - **JPEG** (and PNG/GIF/TIFF when the installed QuickTime exporter has
     them) via the QuickTime Graphics Exporter:
     `OpenADefaultComponent(GraphicsExporterComponentType, kQTFileTypeJPEG, …)`,
     `GraphicsExportSetInputGWorld`, `GraphicsExportSetOutputFile`,
     `GraphicsExportSetCompressionQuality`, `GraphicsExportDoExport`.
     `CompressImage` (IMI) is the lower-level alternative.
   - `PutScrap` if a pasteable Clipboard copy is wanted; `GetScrap` is the
     reverse intake path for user-pasted images.

Notes for the target machine:

- Classic OS cannot enumerate another process's windows (`WindowList` /
  `GetWindowList` are per-process), so "window capture" is screen capture plus
  a crop. The native ⌘⇧Caps Lock-4 shot is interactive only.
- Memory is the one real bound: 1024×768 is 3 MiB at 32-bit, 1.5 MiB at
  16-bit, 768 KiB at 8-bit. `NewGWorld` returns a `QDErr` (handle
  `memFullErr`). Sherclawk's static footprint is 2.33 MiB with a 4 MiB
  minimum / 8 MiB preferred `SIZE`, so capture at reduced depth, in horizontal
  bands, or with temporary-memory flags — or raise `SIZE` deliberately and
  re-measure.
- QuickTime is part of a standard OS 9 install but its version depends on the
  install and updates. Discover the exporter at runtime and fall back to PICT.
  Linkage is also unproven: the `mpw-ppc-v2` descriptor currently allows only
  `InterfaceLib`/`StdCLib`, so a QuickTime library may need to be allowed and
  guest-verified.

## 3. Focus and occlusion — the part people get wrong

The practical rule is **"unobscured and redrawn"**, not "focused":

- Classic Mac OS has no compositor and no per-window backing store. Windows
  are rectangles in one shared framebuffer; `visRgn` clips drawing, and a
  covered window only repaints when it next processes an update event
  (Technical Note 405 is a catalog of how fragile that is). Unlike macOS's
  `CGWindowListCreateImage`, there is no API that returns an occluded window's
  retained image.
- A fully visible background window **can** be captured without stealing
  focus — but it renders inactive chrome (title-bar stripes, gray selection,
  no caret). Fine for "does it fit", wrong for "does it look good in use".
- A partially covered window, or one under Sherclawk's window, must be fronted
  so it repaints. Since occlusion cannot even be computed cross-process,
  fronting is the reliable guarantee.
- Fronting is native and scoped: `SetFrontProcess(&psn)` using the PSN
  `LaunchApplication` already returns (Sherclawk currently passes
  `launchDontSwitch`, which is why the new app stays behind). Then **yield**:
  cooperative switching means the target redraws only when it next calls
  `WaitNextEvent`; `Delay` gives it time under MultiFinder. Capture after a
  bounded wait (and capturing twice, keeping the second, is cheap insurance),
  then optionally `SetFrontProcess` back to Sherclawk. A fronted target is
  above Sherclawk's window, so nothing needs hiding.

## 4. The real blocker: an image channel to the model

- Tool results are ≤1536 bytes and requests ≤288 KiB (`agent.h:5-10`); message
  content is built as a plain JSON string (`agent.c:95`, `:135-146`) and
  parsed as one (`:205-207`). Image bytes cannot ride a tool result; the
  attachment has to be added to the next request's user message.
- OpenRouter-compatible vision models accept
  `{"type":"image_url","image_url":{"url":"data:image/jpeg;base64,…"}}`
  content parts. That means emitting array content, choosing a vision-capable
  model (the session's configured model, not a hardcoded one), and confirming
  the provider accepts data URIs.
- Budget arithmetic, not measured: base64 is 4/3 of the JPEG, so one
  downscaled (≤1024×768) q40–60 JPEG should land well under half the request
  cap — but the 256 KiB history cap means raw images must **not** be retained
  per turn. Prefer: persist the artifact + a text placeholder in history, and
  re-attach only when the model asks (`view_image(path)`) or simply capture
  fresh. This matches the research note already calling for screenshots as
  referenced artifacts (`docs/sherclawk-agent-harness-research.md:340`).
- Until the channel exists, artifacts still deliver most of the non-vision
  value: AFP-share files the user opens, session/handoff evidence, and
  documents for the apps Sherclawk builds.

## 5. Self-render option (apps it generates)

Because the template apps are Sherclawk's own source, they can render their
content into an offscreen GWorld/PICT on request (small custom Apple Event,
same pattern as `kAEQuitApplication` in
`docs/idea-004-sherclawk-os9-tools.md` §3) and/or answer with
their window bounds. That yields a deterministic, content-only image even
when the window is occluded or hidden — arguably better for regression diffs
than a screen shot — at the cost of missing the window frame, system chrome,
and stray alert dialogs. It only works for template-generated apps, which is
exactly the capture → compare → improve loop.

## 6. Other uses

- Visual acceptance and regression after layout edits; before/after evidence.
- Catching crash and modal-error dialogs instead of just `process_present`.
- Studying period-correct UI (SimpleText, QuickTime Player, Appearance
  dialogs) as design references.
- Support and debugging: "what is on screen right now", plus evidence for
  stuck dialogs (e.g. a ToolServer permission prompt blocking automation).
- Clipboard intake: `GetScrap` saves a user-pasted image as a workspace asset.
- Documentation and user-guide images; build time-lapse; handoff evidence.
- Classic OS has no accessibility tree, so pixels plus a vision model is the
  general fallback for understanding any UI. OS 9 has no OCR; the model would
  do that job.

## Recommended sequence

1. `capture_screen` (PICT plus optional JPEG) + automatic post-launch capture,
   artifact-only; guest-verify in the established diagnostic pattern.
2. Template self-render/window-bounds Apple Event, if precise content-only
   shots prove worth it.
3. Wire format + vision model + history policy for the image channel.

## Verification, when built

Per `AGENTS.md`, host success is not guest evidence. Plan a
`SherclawkCaptureCheck`-style diagnostic plus a live model conversation:

- Capture at 8-, 16- and 32-bit screens; confirm coverage, colors and the
  `memFullErr` path; record actual memory and `SIZE` implications.
- Occluded vs. fronted windows; background (inactive) capture; restore-focus
  behavior; menu bar and multi-display handling if available.
- PICT opens in a native viewer; JPEG exporter presence and output size;
  file naming/collision behavior.
- Image attachment fits the request cap; history does not retain raw images;
  a vision model actually describes the captured window.

## Open questions

- Default format and destination: PICT vs JPEG, `Sherclawk Shots:` folder or
  per-build snapshot, naming/collision policy, ticks-based unique names?
- Does capture join the mutation journal as a create-only artifact (staged,
  verified, `uncertain` on failure), or is it a non-mutating diagnostic file?
- Attach policy: one-shot per capture, or a `view_image(path)` re-attach tool?
  What downsample/quality defaults keep under the request cap?
- Which vision model(s) to advertise, and does the config let the session fix
  one for the run as it does today?
- Does `run_application` front the app for the automatic shot, or only on an
  explicit capture request (focus-stealing UX trade-off)?
- Can the `mpw-ppc-v2` descriptor allow the QuickTime library without
  violating the fixed-library policy, or is PICT the only MVP format?
- Does the target 9.2.2 QuickTime include a JPEG (and PNG) graphics exporter?
- Confirm the classic Caps Lock window-shot and Control-to-Clipboard details
  in the guest before documenting them as supported.

## Evidence

- Locally installed, user-supplied Universal Interfaces, `InterfacesAndLibraries/Interfaces/CIncludes/`:
  - `Quickdraw.h` — `CopyBits` :3674, `OpenPicture` :3748, `ClosePicture`
    :3775, `GetDeviceList` :4902, `GetMainDevice` :4914.
  - `QDOffscreen.h` — `NewGWorld` :104 (returns `QDErr`), `LockPixels` :204,
    `GetGWorldPixMap` :500.
  - `ImageCompression.h` — `CompressImage` :1235,
    `GraphicsExporterComponentType` :5066, `GraphicsExportDoExport` :5127,
    `GraphicsExportSetInputGWorld` :5819, `GraphicsExportSetOutputFile` :6057.
  - `Components.h` — `OpenADefaultComponent` :889.
  - `QuickTimeComponents.h` — `kQTFileTypePicture` :1307, `kQTFileTypePNG`
    :1318, `kQTFileTypeJPEG` :1323.
  - `Scrap.h` — `GetScrap` :159, `PutScrap` :186.
  - `Processes.h` — `launchContinue` :80, `launchDontSwitch` :83,
    `LaunchApplication` :238, `SetFrontProcess` :320.
  - `OSUtils.h` — `Delay` :356. `MacWindows.h` — `GetWindowList` :5285
    (per-process).
- Repository: `main.c:454` (`CopyBits` in use),
  `agent.h:5-10` (caps), `agent.c:95`, `:135-146`,
  `:205-207` (string content), `README.md` run_application notes
  (:273-274) and limits/SIZE (:374-394),
  `docs/sherclawk-agent-harness-research.md:340` (screenshots as referenced
  artifacts), `:376` (open question), `:459` (QuickDraw capture item),
  `docs/idea-004-sherclawk-os9-tools.md` §10 (:217).
- Native behavior: [classic keyboard-command
  list](https://devonhubner.org/Macintosh_Classic_Keyboard_Commands) (all
  shortcuts above, Caps Lock window shot marked classic-only); [System 7
  thread](https://68kmla.org/bb/threads/how-do-i-take-a-screenshot-in-system-7.24394)
  (PICT `Picture x` at the startup-disk root); [vintage screenshot
  history](https://aaron.cc/opening-screenshots-from-a-vintage-macintosh)
  (System 6 `Screen 0`); [OS 9.2 PICT
  report](https://www.mac-forums.com/threads/mac-os-9-2-screenshot-help.106445);
  [Technical Note
  405](https://spinsidemacintosh.neocities.org/tn405) (obscured-window update
  fragility).
