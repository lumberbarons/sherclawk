#!/bin/bash
# Compile the app's protocol modules against the proven host TLS socket shim.
# All outputs/staged third-party sources stay under ignored build/.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
HTTPS="$(cd "$HERE/../hello-https" && pwd)"
CERTAINLY_DIR="${CERTAINLY_DIR:-$HERE/../Certainly}"
STAGE="$HERE/build/host-certainly"
mkdir -p "$STAGE"
cp -R "$CERTAINLY_DIR/src" "$CERTAINLY_DIR/include" "$CERTAINLY_DIR/bearssl" "$STAGE/"
git init -q "$STAGE"
for patch in "$HTTPS"/patches/*.patch; do
    (cd "$STAGE" && GIT_CEILING_DIRECTORIES="$STAGE" git apply "$patch")
done
LOCAL_FLAGS=(-DSHERCLAWK_HOST=1)
if [ -f "$HERE/config.local.h" ]; then LOCAL_FLAGS+=(-DSHERCLAWK_HAS_LOCAL_CONFIG=1); fi
SAN_FLAGS=(-fsanitize=address,undefined -fno-omit-frame-pointer)
# bash on macOS is 3.2; populate the array without mapfile.
BEARSSL_SOURCES=()
while IFS= read -r file; do BEARSSL_SOURCES+=("$file"); done < <(find "$STAGE/bearssl/src" -name '*.c' | sort)
"${CC:-cc}" -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    "${SAN_FLAGS[@]}" "${LOCAL_FLAGS[@]}" \
    -I"$HERE" -I"$HTTPS" -I"$HTTPS/tools/host-tls/shim" \
    -I"$STAGE/include" -I"$STAGE/src" -I"$STAGE/bearssl/inc" -I"$STAGE/bearssl/src" \
    "$HERE/tools/probe.c" "$HERE/agent.c" "$HERE/chat.c" "$HERE/json.c" "$HERE/text.c" \
    "$HERE/network.c" "$HTTPS/http.c" \
    "$HTTPS/tools/host-tls/host_transport.c" "$HTTPS/tools/host-tls/host_shim.c" \
    "$STAGE/src/certainly.c" "$STAGE/src/tls13_handshake.c" \
    "$STAGE/src/tls13_keysched.c" "$STAGE/src/tls13_record.c" \
    "$STAGE/src/entropy.c" "$STAGE/src/ca_roots.c" "${BEARSSL_SOURCES[@]}" \
    -o "$HERE/build/host-probe"
echo "Built $HERE/build/host-probe"
