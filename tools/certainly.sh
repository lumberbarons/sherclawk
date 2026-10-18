#!/bin/bash
# The pinned Certainly dependency: the single source of truth for which commit
# of the fetch-only lumberbarons/certainly fork every check and build uses.
#
# Scripts source this file and call certainly_ensure, which moves a clean
# clone to the pinned commit (fetching it by SHA when missing) and refuses to
# touch a checkout with local changes. CI uses certainly_ci_fetch instead,
# which fetches exactly the pin and nothing else. Either way, what CI builds
# is what a local run builds, and a push to the fork cannot change it.
#
# Moving the pin is a deliberate PR: run tools/certainly-bump.sh, let it run
# the suites against the new commit, then commit the updated pin.
#
# For local experiments against another commit, override CERTAINLY_PIN in the
# environment; CI always builds the baked-in value.

CERTAINLY_REPO="https://github.com/lumberbarons/certainly.git"
CERTAINLY_PIN="${CERTAINLY_PIN:-ca2d33def91ef9ba1a6482aa7100f97bbaab19f1}"

# certainly_dir — echo the clone location: CERTAINLY_DIR, or ../Certainly
# next to this repository (the documented default).
certainly_dir() {
    local root
    root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    printf '%s\n' "${CERTAINLY_DIR:-$root/../Certainly}"
}

# certainly_ensure DIR — make the clone at DIR match the pin. Returns
# non-zero, with guidance, when the clone is missing or has local changes;
# never discards work. Callers run with set -e, so a failure stops the run.
certainly_ensure() {
    local dir="$1" head
    if [ ! -f "$dir/include/certainly.h" ] || [ ! -f "$dir/CMakeLists.txt" ]; then
        echo "error: Certainly not found at $dir" >&2
        echo "clone it next to this repository (fetch-only; scripts check out" >&2
        echo "the pinned commit themselves):" >&2
        echo "  git clone --depth 1 $CERTAINLY_REPO Certainly" >&2
        echo "override the location with CERTAINLY_DIR." >&2
        return 1
    fi
    if ! head="$(git -C "$dir" rev-parse HEAD 2>/dev/null)"; then
        echo "error: $dir is not a git checkout; cannot verify the pinned commit" >&2
        return 1
    fi
    [ "$head" = "$CERTAINLY_PIN" ] && return 0
    if [ -n "$(git -C "$dir" status --porcelain --untracked-files=no)" ]; then
        echo "error: $dir has local changes and is not at the pinned commit" >&2
        echo "resolve them (or point CERTAINLY_DIR at a clean clone) so every" >&2
        echo "check and build uses the pinned commit ${CERTAINLY_PIN:0:7}." >&2
        return 1
    fi
    if ! git -C "$dir" cat-file -e "$CERTAINLY_PIN^{commit}" 2>/dev/null; then
        echo "Certainly: fetching pinned commit ${CERTAINLY_PIN:0:7} ..." >&2
        # Fetch from the pinned repository URL, not the clone's origin: a
        # clone of upstream would not carry the fork's commits.
        if ! git -C "$dir" fetch --quiet --depth 1 "$CERTAINLY_REPO" "$CERTAINLY_PIN"; then
            echo "error: could not fetch the pinned commit ${CERTAINLY_PIN:0:7}" >&2
            echo "from $CERTAINLY_REPO; check the network or clone the fork fresh." >&2
            return 1
        fi
    fi
    git -C "$dir" checkout --quiet --detach "$CERTAINLY_PIN" || return 1
    echo "Certainly: checked out pinned commit ${CERTAINLY_PIN:0:7}" >&2
}

# certainly_ci_fetch DIR — fresh checkout of exactly the pinned commit for a
# runner with no clone (GitHub serves any reachable SHA to a fetch).
certainly_ci_fetch() {
    local dir="$1"
    rm -rf "$dir"
    git init --quiet "$dir"
    git -C "$dir" remote add origin "$CERTAINLY_REPO"
    git -C "$dir" fetch --quiet --depth 1 origin "$CERTAINLY_PIN"
    git -C "$dir" checkout --quiet --detach "$CERTAINLY_PIN"
    test "$(git -C "$dir" rev-parse HEAD)" = "$CERTAINLY_PIN"
}
