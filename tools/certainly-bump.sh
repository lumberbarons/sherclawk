#!/bin/bash
# Bump the pinned Certainly commit to the fork's current main. This is how a
# new Certainly version is taken: the pin change is committed and reviewed in
# a PR of its own, never pulled in implicitly by a rebuild.
#
# Fetches the fork's main into the local clone, runs the full host suites
# against it, and rewrites the pin in tools/certainly.sh. The suites gate the
# update: on failure the pin file is restored (other local edits kept) and
# nothing is staged. The script never commits or pushes.
#
# Usage: tools/certainly-bump.sh [--check]
#   --check  report the fork's current main without touching anything
#
# Host suites cover the protocol, loop and TLS I/O surfaces, not guest
# behavior; a bump whose fork commits change runtime behavior (entropy,
# buffer bounds, record limits) still needs its own guest verification before
# the PR merges.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
. "$HERE/tools/certainly.sh"
DIR="$(certainly_dir)"

if [ ! -d "$DIR/.git" ]; then
    echo "error: no Certainly clone at $DIR (override with CERTAINLY_DIR)" >&2
    exit 1
fi
if [ -n "$(git -C "$DIR" status --porcelain --untracked-files=no)" ]; then
    echo "error: $DIR has local changes; resolve them first" >&2
    exit 1
fi

old="$CERTAINLY_PIN"
git -C "$DIR" fetch --quiet "$CERTAINLY_REPO" main
new="$(git -C "$DIR" rev-parse FETCH_HEAD)"
if [ "$new" = "$old" ]; then
    echo "Already pinned at ${old:0:7}, the fork's current main."
    exit 0
fi
echo "Fork main moved: ${old:0:7} -> ${new:0:7}"
git -C "$DIR" log --oneline --no-decorate "$old..$new" 2>/dev/null | sed 's/^/  /' ||
    echo "  (previous pin ${old:0:7} is no longer on the fork's main history)"

if [ "${1:-}" = "--check" ]; then
    echo "dry run: no pin change (run tools/certainly-bump.sh to take it)"
    exit 0
fi

git -C "$DIR" checkout --quiet --detach "$new"
tmp="$(mktemp)"
sed "s|^CERTAINLY_PIN=.*|CERTAINLY_PIN=\"$new\"|" "$HERE/tools/certainly.sh" > "$tmp"
mv "$tmp" "$HERE/tools/certainly.sh"
grep -q "^CERTAINLY_PIN=\"$new\"$" "$HERE/tools/certainly.sh" || {
    echo "error: pin update failed" >&2
    exit 1
}

if "$HERE/tools/check.sh" && "$HERE/tools/check-transport.sh"; then
    echo
    echo "Suites pass against ${new:0:7}. Commit tools/certainly.sh and open a"
    echo "PR; CI builds the pinned commit, so merging takes the new version."
    echo "Guest-visible fork changes still need their own OS 9 verification."
else
    status=$?
    sed "s|^CERTAINLY_PIN=.*|CERTAINLY_PIN=\"$old\"|" "$HERE/tools/certainly.sh" > "$tmp"
    mv "$tmp" "$HERE/tools/certainly.sh"
    echo "error: suites failed against ${new:0:7}; pin restored to ${old:0:7}" >&2
    exit "$status"
fi
