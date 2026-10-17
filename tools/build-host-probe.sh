#!/bin/bash
# Compile the app's protocol modules against the proven host TLS socket shim.
# All outputs stay under ignored build/.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
CERTAINLY_DIR="${CERTAINLY_DIR:-$HERE/../Certainly}"
if [ ! -d "$CERTAINLY_DIR/include" ]; then
    echo "error: Certainly not found at $CERTAINLY_DIR" >&2
    echo "clone it next to this repository (fetch-only):" >&2
    echo "  git clone --depth 1 https://github.com/lumberbarons/certainly.git Certainly" >&2
    exit 1
fi
mkdir -p "$HERE/build"
LOCAL_FLAGS=(-DSHERCLAWK_HOST=1 -DSHERCLAWK_APP=1)
if [ -f "$HERE/config.local.h" ]; then LOCAL_FLAGS+=(-DSHERCLAWK_HAS_LOCAL_CONFIG=1); fi
# shellcheck disable=SC2054  # the comma is part of -fsanitize=address,undefined
SAN_FLAGS=(-fsanitize=address,undefined -fno-omit-frame-pointer)
# bash on macOS is 3.2; populate the array without mapfile.
BEARSSL_SOURCES=()
while IFS= read -r file; do BEARSSL_SOURCES+=("$file"); done < <(find "$CERTAINLY_DIR/bearssl/src" -name '*.c' | sort)
"${CC:-cc}" -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    "${SAN_FLAGS[@]}" "${LOCAL_FLAGS[@]}" \
    -I"$HERE" -I"$HERE/src" -I"$HERE/vendor" -I"$HERE/vendor/host-tls/shim" \
    -I"$CERTAINLY_DIR/include" -I"$CERTAINLY_DIR/src" -I"$CERTAINLY_DIR/bearssl/inc" -I"$CERTAINLY_DIR/bearssl/src" \
    "$HERE/tools/probe.c" "$HERE/src/agent/agent.c" "$HERE/src/app/chat.c" "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/registry.c" "$HERE/src/agent/text.c" \
    "$HERE/src/agent/network.c" "$HERE/vendor/http.c" \
    "$HERE/vendor/host-tls/host_transport.c" "$HERE/vendor/host-tls/host_shim.c" \
    "$CERTAINLY_DIR/src/certainly.c" "$CERTAINLY_DIR/src/tls13_handshake.c" \
    "$CERTAINLY_DIR/src/tls13_keysched.c" "$CERTAINLY_DIR/src/tls13_record.c" \
    "$CERTAINLY_DIR/src/entropy.c" "$CERTAINLY_DIR/src/ca_roots.c" "${BEARSSL_SOURCES[@]}" \
    -o "$HERE/build/host-probe"
echo "Built $HERE/build/host-probe"
