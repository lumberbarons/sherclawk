# Sherclawk public release plan

> Note (October 7, 2026): the **internal** export was executed via
> [STANDALONE.md](STANDALONE.md), which is now the operative plan. Two facts
> below are stale: `hello-https` ships **8** Certainly patches (not 7), and
> `sherclawk/` carries its full commit history (not a single commit).

Goal: publish `sherclawk/` as a self-contained public GitHub project — no
credentials, no machine-specific identity, and a build that works from a
fresh clone. Findings below were verified on 2026-10-02.

## Verified current state

- History is clean: `git log --all -S 'sk-or-'` finds no key and no secret
  has ever been committed. `git check-ignore -v sherclawk/config.local.h`
  confirms the local config is ignored, not tracked.
- `sherclawk/config.local.h` (ignored) holds a live OpenRouter API key, and
  the same key is in `hello-chat/config.local.h`. The key is compiled into
  25+ artifacts under `sherclawk/build/` and 19 under `hello-chat/build/`.
- `sherclawk/build/` (32 MB, ignored) also contains `.git` clones of
  Certainly (`host-certainly`, `transport-tests`), `.dsk` images, host test
  binaries and `__pycache__/`.
- A single sherclawk commit exists: `34fbe32`, authored by
  `lumberbarons <jellwood@gmail.com>`.
- The folder does not build or test outside this workspace: scripts reach
  into sibling directories (table below). `../sherclawk.png` (the artwork
  source) is ignored even by the workspace root and is required by
  `build.sh`.
- No `LICENSE` file. Certainly is MIT (© 2026 Matt Baker), BearSSL MIT,
  Retro68 GPL3+ (toolchain only, not redistributed), and Apple's Universal
  Interfaces must never be redistributed — they stay fetch-only.
- Host-specific details: `SHARE_HOST=beardmore` default in
  `tools/deploy-to-share.sh`, `ssh beardmore` and `/srv/retro68` in
  `README.md`, and `tools/guest-input.py` imports `utm_qmp` from
  `macos922/tools/`.
- Adjacent assets at the workspace root, not in this folder:
  `sherclawk-agent-harness-research.md` and `sherclawk.png`.

## Export mechanics (do first, safety)

- [ ] Rotate the OpenRouter key; update `sherclawk/config.local.h` and
      `hello-chat/config.local.h`. Treat the old key as burned.
- [ ] Never `cp -r` the working tree. Export tracked files only:
      `git archive 34fbe32 sherclawk | tar -x -C ../sherclawk-public`, or
      `git subtree split -P sherclawk` if the one-commit history is kept.
- [ ] Add a standalone `.gitignore`: `build/`, `config.local.h`,
      `__pycache__/`, `*.py[cod]`, `.DS_Store`, `._*`, `*.dsk`, `*.img`,
      `*.log`.
- [ ] Decide history: a fresh single commit avoids publishing the author
      email; keeping `34fbe32` is otherwise safe (no secrets in it).
- [ ] After the first push, verify nothing sensitive is tracked:
      `git ls-files | grep -E 'build/|config\.local\.h|__pycache__'` must
      return nothing.

## Standalone build work

Every consumer that currently reaches outside the folder:

| Consumer | Needs from workspace | Proposal |
|---|---|---|
| `CMakeLists.txt` | `../Certainly` (`add_subdirectory`), `../hello-https/http.c`, include `../hello-https` | Vendor `http.c`/`http.h` under e.g. `vendor/`; keep `CERTAINLY_DIR` overridable with a documented clone step |
| `build.sh` | `../hello-https/build.sh` (Docker Retro68, Certainly staging, Universal Interfaces fetch, 7 patches), `../sherclawk.png` | Copy/adapt the ~100-line build script into the folder; keep patches local; see artwork below |
| `tools/check.sh` | `../hello-https/http.c`, `../Certainly/include`, `../hello-https/tools/host-tls/shim` | Vendored `http.c` + the small shim headers |
| `tools/check-transport.sh` | `../hello-https/patches/*.patch`, Certainly clone | Vendor the 7 patches; keep Certainly a clone-in-build or submodule |
| `tools/build-host-probe.sh` | same as above | Same |
| `tools/guest-input.py` | `utm_qmp` from `macos922/tools/` | Vendor the helper or drop the tool from the public tree |
| `build.sh` art step | `../sherclawk.png` (8-bit RGBA, noninterlaced) | Include the PNG (force-add or relocate) or make the art step optional with a placeholder/default icon |

- [ ] Vendor `http.c`/`http.h`, the host-tls shim, the Certainly patches,
      and (if kept) `utm_qmp.py`.
- [ ] Adapt `build.sh` into a self-contained staging script: clone check
      for Certainly, fetch Universal Interfaces, stage with local patches,
      build in `ghcr.io/autc04/retro68`. Keep upstream Certainly pristine,
      as today.
- [ ] Point `CMakeLists.txt` and the check scripts at the vendored paths.
- [ ] Decide whether Certainly becomes a git submodule or a documented
      clone step (the current script already handles the latter).
- [ ] Include the artwork or make it optional; update `README.md`
      accordingly.
- [ ] Decide whether `sherclawk.png` and
      `sherclawk-agent-harness-research.md` belong in the public repo.

## LICENSE and attribution

- [ ] Choose a license (MIT pairs naturally with this stack) and a
      copyright holder.
- [ ] Add third-party notices: Certainly MIT (© 2026 Matt Baker) and
      BearSSL MIT if staged/vendored; note Retro68 GPL3+ as a build
      prerequisite, and that Apple Universal Interfaces are fetched, never
      committed.
- [ ] Confirm the artwork is original or AI-generated and safe to license,
      including the "Sherclawk" name and lobster mascot likeness.

## Sanitization

- [ ] Remove the personal hostname default: require `SHARE_HOST` or use a
      placeholder in `tools/deploy-to-share.sh`.
- [ ] Replace `ssh beardmore` / `/srv/retro68` in `README.md` with
      placeholders.
- [ ] Rewrite `README.md` for a standalone audience: prerequisites
      (Docker, Retro68 image, Certainly clone, Universal Interfaces fetch),
      drop workspace-only references (`../hello-chat/`, root README, UTM
      specifics can stay as optional diagnostics).
- [ ] Keep `PLAN.md` (roadmap) but review it for workspace-specific
      assumptions before publishing.

## Clean-room verification

- [ ] Build and run `tools/check.sh` and `tools/check-transport.sh` from a
      fresh checkout outside this workspace (ASan/UBSan, no VM/key).
- [ ] Run `tools/build-host-probe.sh` plus a live probe with a local key;
      the host probe checks real provider protocol and transport.
- [ ] Build the PowerPC app from the clean checkout using the Docker
      Retro68 image.
- [ ] Guest verification (Finder icon, tool loop) stays optional and
      documented; it needs a netatalk share and a VM and cannot be
      reproduced from the repo alone.
- [ ] Pre-push secret scan:
      `git grep -nE 'sk-or-|ghp_|AKIA|-----BEGIN'` on tracked files, plus
      gitleaks/trufflehog if available.

## Publish

- [ ] Create the public repository and push.
- [ ] Enable GitHub secret scanning and push protection.
- [ ] Re-verify the remote tree has no `build/`, no `config.local.h`, and
      no key material.

## Decisions needed

1. License and copyright holder.
2. Keep history (`34fbe32`, exposes the author email) or start fresh.
3. Vendor Certainly as a submodule vs a clone step in `build.sh`.
4. Include `sherclawk.png` and `sherclawk-agent-harness-research.md`.
