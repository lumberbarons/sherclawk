# Sherclawk standalone — internal export plan

Status: **executed** (October 7, 2026).
Target: private `github.com/lumberbarons/sherclawk`, history preserved with
`git subtree split` (33 commits touch `sherclawk/`; first is `34fbe32`).

Companion to `PUBLIC-RELEASE.md` (the public-release variant). Two updates to
that document: `hello-https` now ships **8** Certainly patches (it says 7), and
there are **uncommitted self-build changes** (`selfbuild.c`, `build_plan.h`,
`tests/test_selfbuild.c`, 13 modified files) that must be committed before any
export. For an internal repo, author-email and hostname sanitization are
optional; secret hygiene is not.

Decisions already made:

- Contain the folder **in place first** in this workspace, verify here, then
  export — so there is one source of truth and the workspace rig keeps working.
- Keep history via `git subtree split`.
- Move into the repo: artwork (`sherclawk.png`), idea 005, and the research doc.

Resolved open decisions (October 7, 2026):

- **Development home:** the standalone clone at `/Users/jim/projects/sherclawk`
  (the imac copy freezes after export).
- **SHARE_HOST:** required env var in `tools/deploy-to-share.sh`; docs use
  `$SHARE_HOST` placeholders.
- **LICENSE:** MIT ("Copyright (c) 2026 lumberbarons") plus
  `THIRD-PARTY-NOTICES.md`.
- **Certainly:** documented clone step (no submodule).
- **OpenRouter key:** not rotated (never committed; accepted risk).

`git subtree` (Apple Git 2.50.1) and authenticated `gh` (account
`lumberbarons`, `repo` scope) were verified on this machine.

## 1. Preconditions (imac repo)

- [ ] Commit the in-flight self-build work so the export includes it:
      `git add sherclawk && git commit -m "<message>"` (message is yours;
      everything under `sherclawk/` is self-contained).
- [ ] Confirm only this plan is left untracked:
      `git status --short sherclawk/`.
- [ ] Recommended: rotate the OpenRouter key in `sherclawk/config.local.h`
      and `hello-chat/config.local.h`. It has never been committed
      (`git log --all -S 'sk-or-'` is empty), but it is compiled into 25+
      ignored artifacts under `build/`. Rotation is the cheapest insurance.
- [ ] Do **not** touch `build/` (32 MB, ignored; contains the key, `.git`
      clones of Certainly and `.dsk` images). Exports use tracked files only.

## 2. Vendor dependencies and assets

Copy source → destination (create `vendor/`, `patches/`, `art/`, `docs/`):

| Source in this workspace | Destination in `sherclawk/` | Why |
|---|---|---|
| `hello-https/http.c`, `http.h` | `vendor/http.c`, `vendor/http.h` | CMake, `check.sh`, host probe |
| `hello-https/patches/*.patch` (8 files) | `patches/` | Docker staging, transport checks |
| `hello-https/tools/host-tls/shim/*.h` (7 headers) | `vendor/host-tls/shim/` | `check.sh`, transport checks, probe |
| `hello-https/tools/host-tls/host_transport.c`, `host_shim.c` | `vendor/host-tls/` | host probe |
| `hello-https/tools/host-tls/README.md` | `vendor/host-tls/README.md` | documents the shim (optional) |
| `hello-https/tools/get-universal-interfaces.sh` | `tools/get-universal-interfaces.sh` | one-time fetch; adjust destination/help text |
| `macos922/tools/utm_qmp.py`, `qmpdrive.py` | `tools/` | `tools/guest-input.py` |
| `sherclawk.png` (workspace root, 1254×1254 RGBA) | `art/sherclawk.png` | default art source for `build.sh` |
| `ideas/005-sherclawk-queue-mode.md` | `docs/idea-005-sherclawk-queue-mode.md` | README links (3 places) |
| `sherclawk-agent-harness-research.md` | `docs/sherclawk-agent-harness-research.md` | research record |

Command shape: `mkdir -p vendor/host-tls/shim patches art docs` then `cp`
each row. Never copy `build/`, `config.local.h`, or `__pycache__/`.

- [ ] Add `!sherclawk/art/sherclawk.png` to the workspace root `.gitignore`
      (its `*.png` rule would otherwise ignore the artwork).
- [ ] Certainly and `InterfacesAndLibraries` remain **fetch-only** — never
      committed (Apple's interfaces must not be redistributed).

## 3. Script rewrites

### `build.sh` — absorb the `hello-https/build.sh` staging

Current 9-line delegator becomes a ~100-line self-contained script (copy
`hello-https/build.sh` as the template, then adjust):

- [ ] Embed template; `make-art.py` from `${SHERCLAWK_ART:-$HERE/art/sherclawk.png}`
      to `build/art.r`.
- [ ] Defaults with actionable errors: `CERTAINLY_DIR=$HERE/../Certainly`
      (clone hint), `INTERFACES_DIR=$HERE/../InterfacesAndLibraries`
      (point at local `tools/get-universal-interfaces.sh`), `PATCH_DIR=$HERE/patches`.
- [ ] Docker run keeps the image (`ghcr.io/autc04/retro68`), the
      interfaces-and-libraries swap, Certainly patch staging into `/tmp`
      (upstream stays pristine), `touch`, and `cmake -DCERTAINLY_DIR=/tmp/certainly`.
- [ ] Mounts: `$HERE:/work`, `$PATCH_DIR:/patches:ro`,
      `$CERTAINLY_DIR:/certainly:ro`, `$INTERFACES_DIR:/interfaces:ro`
      (drop the old `/hello-https` mount).
- [ ] Keep `BUILD_TARGET` argument, default `Sherclawk_APPL`, and the final
      artifact listing.

### Other scripts

- [ ] `CMakeLists.txt`: `CHAT_CORE` uses `vendor/http.c` (was
      `../hello-https/http.c`); include dirs `. vendor` (was `../hello-https`);
      `CERTAINLY_DIR` default stays `../Certainly` (document the clone).
- [ ] `tools/check.sh`: replace the two `../hello-https` include/source refs
      with `$HERE/vendor` and `$HERE/vendor/http.c`; the network test also
      needs `-I"${CERTAINLY_DIR:-$HERE/../Certainly}/include"` and
      `-I"$HERE/vendor/host-tls/shim"`. (Requires a Certainly clone;
      consider skipping that test with a clear message when it's absent.)
- [ ] `tools/check-transport.sh`: `PATCH_DIR="$HERE/patches"`,
      `SOURCE="${CERTAINLY_DIR:-$HERE/../Certainly}"`, shim from `$HERE/vendor/host-tls/shim`.
- [ ] `tools/build-host-probe.sh`: same CERTAINLY/PATCH handling; compile
      `$HERE/vendor/http.c`, `$HERE/vendor/host-tls/host_transport.c`,
      `$HERE/vendor/host-tls/host_shim.c`; include `$HERE/vendor` and the shim.
- [ ] `tools/guest-input.py`: delete the `parents[2] / 'macos922/tools'`
      sys.path insert (vendored `utm_qmp.py` inserts its own directory for
      `qmpdrive.py`); update the docstring.
- [ ] `tools/deploy-to-share.sh`: mechanically unchanged; host default is an
      open decision (below).

## 4. Docs and housekeeping

- [ ] `README.md`: drop `../hello-chat/` and `../sherclawk.png` references
      (lines 3–5); rewrite the build section as "ships its own Certainly
      staging (vendored patches), fetch script, art at `art/sherclawk.png`"
      with prerequisites (Docker + Retro68 image, Certainly clone, interfaces
      fetch); repoint the 3 `../ideas/005…` links at `docs/idea-005…`
      (anchors unchanged); optionally strip `sherclawk/` path prefixes in the
      verification section and replace `ssh beardmore` examples with
      `$SHARE_HOST` placeholders.
- [ ] `templates/ppc-toolbox/README.md`: repoint
      `../../../ideas/005…` → `../../docs/idea-005…`.
- [ ] `docs/idea-005…`: ~15 prose refs use the `sherclawk/` prefix; either
      rewrite root-relative or add a provenance note at the top.
- [ ] `docs/sherclawk-agent-harness-research.md`: fix the one relative link at
      ~line 771 (`sherclawk/templates/…` → `../templates/…`).
- [ ] `PUBLIC-RELEASE.md`: keep as the public variant; add a pointer to this
      file (and note its patch count is stale).
- [ ] New repo `.gitignore`:

      build/
      config.local.h
      __pycache__/
      *.py[cod]
      .DS_Store
      ._*
      *.dsk
      *.img
      *.log

  (No blanket `*.png` — `art/` is tracked.)
- [ ] Optional (recommended): `LICENSE` + third-party notices — Certainly MIT
      © 2026 Matt Baker, BearSSL MIT; Retro68 GPL3+ is a build-image
      prerequisite, not redistributed; Universal Interfaces fetched, never
      committed.

## 5. Verify in place

- [ ] No live references to the old siblings remain:
      `grep -rn "hello-https" sherclawk --include='*.sh' --include='*.c' --include='*.h' --include='CMakeLists.txt'`
      (only doc history may mention it).
- [ ] `sherclawk/tools/check.sh`
- [ ] `sherclawk/tools/check-transport.sh`
- [ ] `sherclawk/tools/build-host-probe.sh`
- [ ] `sherclawk/build.sh` (Docker; PowerPC artifact)
- [ ] Optional but recommended: deploy and launch in OS 9 once, as today.

## 6. Export via subtree

After everything above is committed in the imac repo:

```bash
git subtree split -P sherclawk -b sherclawk-export
git worktree add "$TMPDIR/sherclawk-export" sherclawk-export
cd "$TMPDIR/sherclawk-export"
```

- [ ] Secret scan and inventory (must be empty / clean):
      `git grep -nE 'sk-or-|ghp_|AKIA|-----BEGIN'`
      `git ls-files | grep -E 'build/|config\.local\.h|__pycache__'`
      `git ls-files | wc -l`; confirm `art/sherclawk.png` is present.
- [ ] Push as `main`:

```bash
git branch -m main
gh repo create lumberbarons/sherclawk --private --source . --remote origin --push
```

  (Swap host/org if "internal" means GitHub Enterprise. Fallback without
  `--source`: create the repo, then
  `git remote add origin git@github.com:lumberbarons/sherclawk.git && git push -u origin main`.)

## 7. Clean-room verification

From a fresh clone outside the workspace (the real acceptance test):

```bash
git clone https://github.com/lumberbarons/sherclawk.git "$TMPDIR/sherclawk-clean"
cd "$TMPDIR/sherclawk-clean"
git clone --recursive --depth 1 https://github.com/minorbug/certainly.git ../Certainly
tools/get-universal-interfaces.sh
tools/check.sh
tools/check-transport.sh
tools/build-host-probe.sh
./build.sh          # Docker must be running
```

- [ ] All checks pass; `build/art.r` derives from committed artwork.
- [ ] Live probe is optional (needs a key in `config.local.h`).
- [ ] Re-run the secret scan on the clone.

## 8. Post-push

- [ ] Enable secret scanning / push protection if the plan allows.
- [ ] Confirm the remote clone has no `build/`, `config.local.h`, or key.
- [ ] Clean up: `git worktree remove "$TMPDIR/sherclawk-export"` and
      `git branch -D sherclawk-export` (content now lives on GitHub).
- [ ] Decide where development continues: this workspace (push updates with
      `git subtree split`/`push` when ready) or the standalone clone.

## Open decisions

1. Certainly: documented clone step (recommended — matches current scripts)
   vs git submodule.
2. `SHARE_HOST` default: keep `beardmore` vs require the env var / placeholder.
3. LICENSE and notices: add for the internal repo? (Cheap; MIT deps already
   imply attribution.)
4. Development home after export (see §8).

## Effort and footprint

Roughly half a day of mechanical work plus clean-room build runs. Vendored
footprint ~1.1 MB: ~150 KB vendored source, 842 KB artwork, ~100 KB docs.
