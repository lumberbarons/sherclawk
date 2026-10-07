#!/bin/bash
#
# get-universal-interfaces.sh — fetch Apple's Universal Interfaces for Retro68.
#
# Why: Retro68's container image ships the open-source *Multiversal*
# Interfaces, which deliberately have no Open Transport headers ("missing
# things include ... MacTCP, OpenTransport ...", Retro68 README). Certainly
# needs Apple's *Universal* Interfaces 3.4 to build its Open Transport
# transport layer, and Retro68 supports swapping them in (the container's
# interfaces-and-libraries.sh script).
#
# The files are Apple's and are not redistributed by this repository or the
# container image; this script downloads the Macintosh Garden archive of the
# "Interfaces&Libraries" folder from the MPW 3.5 Golden Master and verifies
# its published MD5. Retro68's own README points at the same kind of source.
#
# Usage:  tools/get-universal-interfaces.sh [target-dir]
#         (default target: ../InterfacesAndLibraries, next to this repo)
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$HERE/../InterfacesAndLibraries}"
URL="${INTERFACES_URL:-https://old.mac.gdn/apps/InterfacesAndLibraries.zip}"
MD5_EXPECTED="c28aaf23195679f9849adf12b0381f6e"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "Downloading $URL ..."
curl -L --fail --progress-bar -o "$tmp/ial.zip" "$URL"

if command -v md5 >/dev/null 2>&1; then
    actual="$(md5 -q "$tmp/ial.zip")"
else
    actual="$(md5sum "$tmp/ial.zip" | cut -d' ' -f1)"
fi
if [ "$actual" != "$MD5_EXPECTED" ]; then
    echo "error: MD5 mismatch (got $actual, expected $MD5_EXPECTED)" >&2
    exit 1
fi

echo "Unpacking..."
unzip -q "$tmp/ial.zip" -d "$tmp/extract"
rm -rf "$DEST"
mv "$tmp/extract/Interfaces&Libraries" "$DEST"

echo "Installed Universal Interfaces to $DEST"
echo "build.sh will pick them up automatically."