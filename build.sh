#!/bin/bash
# Cross-compile Sherclawk for classic Mac OS 9 (PowerPC) using Retro68 —
# including the Certainly TLS library it depends on.
#
# Two things beyond the usual Retro68 container are required:
#
#   * Certainly, a separate upstream clone (fetch-only, never committed):
#       git clone --recursive --depth 1 https://github.com/minorbug/certainly.git Certainly
#     Default location: ../Certainly next to this repo; override CERTAINLY_DIR.
#
#   * Apple's Universal Interfaces (also fetch-only). The container image
#     only ships the open-source Multiversal Interfaces, which have no
#     Open Transport headers — Certainly needs the Universal ones:
#       tools/get-universal-interfaces.sh
#
# The interfaces are swapped into the container's toolchain for the build
# (interfaces-and-libraries.sh, the mechanism Retro68 provides for this),
# and Certainly is staged into the container with the vendored patches
# (patches/*.patch) applied there — the upstream clone stays pristine.
# It is built out-of-tree as part of the same CMake run.
# The artwork resource is generated from art/sherclawk.png (8-bit RGBA,
# noninterlaced); override with SHERCLAWK_ART.
# BUILD_TARGET (or $1) selects a diagnostic target; default Sherclawk_APPL.
#
# Output lands in ./build/:
#   Sherclawk.bin   MacBinary file (data + resource forks)
#   Sherclawk.dsk   raw HFS disk image containing the app
#   Sherclawk.APPL  raw application (needs fork-aware transport)
#
set -euo pipefail
cd "$(dirname "$0")"
HERE="$PWD"

IMAGE="ghcr.io/autc04/retro68"
TOOLCHAIN="/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake"
BUILD_TARGET="${BUILD_TARGET:-${1:-Sherclawk_APPL}}"
PATCH_DIR="$HERE/patches"

CERTAINLY_DIR="${CERTAINLY_DIR:-$HERE/../Certainly}"
if [ ! -f "$CERTAINLY_DIR/CMakeLists.txt" ]; then
    echo "error: Certainly not found at $CERTAINLY_DIR" >&2
    echo "clone it first, next to this repository:" >&2
    echo "  git clone --recursive --depth 1 https://github.com/minorbug/certainly.git Certainly" >&2
    exit 1
fi
CERTAINLY_DIR="$(cd "$CERTAINLY_DIR" && pwd)"

INTERFACES_DIR="${INTERFACES_DIR:-$HERE/../InterfacesAndLibraries}"
if [ ! -f "$INTERFACES_DIR/Interfaces/CIncludes/OpenTransport.h" ]; then
    echo "error: Apple's Universal Interfaces not found at $INTERFACES_DIR" >&2
    echo "fetch them first (one-time):" >&2
    echo "  tools/get-universal-interfaces.sh" >&2
    exit 1
fi
INTERFACES_DIR="$(cd "$INTERFACES_DIR" && pwd)"

# Embed the native project template and convert the artwork before Docker
# starts, so missing inputs fail fast and cleanly.
python3 "$HERE/tools/embed-project-template.py"
python3 "$HERE/tools/make-art.py" "${SHERCLAWK_ART:-$HERE/art/sherclawk.png}" "$HERE/build/art.r"

docker run --rm \
    -e BUILD_TARGET="$BUILD_TARGET" \
    -v "$HERE":/work \
    -v "$PATCH_DIR":/patches:ro \
    -v "$CERTAINLY_DIR":/certainly:ro \
    -v "$INTERFACES_DIR":/interfaces:ro \
    -w /work "$IMAGE" bash -c "
    set -e
    # Swap the container's Multiversal Interfaces for Apple's Universal
    # ones (Open Transport headers and import libraries). 68K and Carbon
    # are off — this is a PowerPC app.
    cd /Retro68-build/bin
    ./interfaces-and-libraries.sh /Retro68-build/toolchain /interfaces false true false
    # Stage Certainly and apply the vendored patches (patches/*.patch).
    # The upstream clone stays pristine and pull-able.
    rm -rf /tmp/certainly
    cp -a /certainly /tmp/certainly
    cd /tmp/certainly
    for p in /patches/*.patch; do
        [ -e \"\$p\" ] || continue
        echo \"applying patch: \$(basename \$p)\"
        git apply \"\$p\"
    done
    # A baseline build may restore older source/header mtimes than the
    # previous optimized objects. Recompile the staged library to avoid
    # mixing context layouts from different patch sets.
    find src include -type f -exec touch {} +
    # Build Certainly + BearSSL + the app.
    cd /work
    mkdir -p build
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release \
             -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN \
             -DCERTAINLY_DIR=/tmp/certainly
    make -j\$(nproc) \"\$BUILD_TARGET\"
"

echo
cd "$HERE"
echo "Built:"
APP="${BUILD_TARGET%_APPL}"
ls -l "build/$APP.bin" "build/$APP.dsk" "build/$APP.APPL" 2>/dev/null || ls -l build/
