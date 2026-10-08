#!/bin/bash
# Compile the app's protocol modules against the proven host TLS socket shim.
# All outputs/staged third-party sources stay under ignored build/.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
CERTAINLY_DIR="${CERTAINLY_DIR:-$HERE/../Certainly}"
if [ ! -d "$CERTAINLY_DIR/include" ]; then
    echo "error: Certainly not found at $CERTAINLY_DIR" >&2
    echo "clone it next to this repository (fetch-only):" >&2
    echo "  git clone --recursive --depth 1 https://github.com/minorbug/certainly.git Certainly" >&2
    exit 1
fi
STAGE="$HERE/build/host-certainly"
mkdir -p "$STAGE"
cp -R "$CERTAINLY_DIR/src" "$CERTAINLY_DIR/include" "$CERTAINLY_DIR/bearssl" "$STAGE/"
git init -q "$STAGE"
for patch in "$HERE"/patches/*.patch; do
    (cd "$STAGE" && GIT_CEILING_DIRECTORIES="$STAGE" git apply "$patch")
done
LOCAL_FLAGS=(-DSHERCLAWK_HOST=1 -DSHERCLAWK_APP=1)
if [ -f "$HERE/config.local.h" ]; then LOCAL_FLAGS+=(-DSHERCLAWK_HAS_LOCAL_CONFIG=1); fi
# shellcheck disable=SC2054  # the comma is part of -fsanitize=address,undefined
SAN_FLAGS=(-fsanitize=address,undefined -fno-omit-frame-pointer)
# bash on macOS is 3.2; populate the array without mapfile.
BEARSSL_SOURCES=()
while IFS= read -r file; do BEARSSL_SOURCES+=("$file"); done < <(find "$STAGE/bearssl/src" -name '*.c' | sort)
"${CC:-cc}" -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    "${SAN_FLAGS[@]}" "${LOCAL_FLAGS[@]}" \
    -I"$HERE" -I"$HERE/vendor" -I"$HERE/vendor/host-tls/shim" \
    -I"$STAGE/include" -I"$STAGE/src" -I"$STAGE/bearssl/inc" -I"$STAGE/bearssl/src" \
    "$HERE/tools/probe.c" "$HERE/agent.c" "$HERE/chat.c" "$HERE/json.c" "$HERE/text.c" \
    "$HERE/network.c" "$HERE/vendor/http.c" \
    "$HERE/vendor/host-tls/host_transport.c" "$HERE/vendor/host-tls/host_shim.c" \
    "$STAGE/src/certainly.c" "$STAGE/src/tls13_handshake.c" \
    "$STAGE/src/tls13_keysched.c" "$STAGE/src/tls13_record.c" \
    "$STAGE/src/entropy.c" "$STAGE/src/ca_roots.c" "${BEARSSL_SOURCES[@]}" \
    -o "$HERE/build/host-probe"
echo "Built $HERE/build/host-probe"
