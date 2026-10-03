#!/bin/bash
# Fake OT/crypto boundaries exercise actual patched MacTLS entry points under
# ASan/UBSan. A private staging repo prevents git apply skipping ignored paths.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
HTTPS="$HERE/../hello-https"
SOURCE="${CERTAINLY_DIR:-$HERE/../Certainly}"
STAGE="$HERE/build/transport-tests"
mkdir -p "$STAGE"
cp -R "$SOURCE/src" "$SOURCE/include" "$STAGE/"
git init -q "$STAGE"
for patch in "$HTTPS"/patches/*.patch; do (cd "$STAGE" && git apply "$patch"); done
LINK_GC=-Wl,--gc-sections
if [ "$(uname -s)" = Darwin ]; then LINK_GC=-Wl,-dead_strip; fi
"${CC:-cc}" -std=gnu99 -O1 -g -Wall -Wextra \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -ffunction-sections -fdata-sections \
    -I"$HTTPS/tools/host-tls/shim" -I"$STAGE/include" -I"$STAGE/src" \
    -I"$SOURCE/bearssl/inc" "$HERE/tests/test_tls_io.c" "$STAGE/src/certainly.c" \
    "$LINK_GC" -o "$STAGE/test-tls-io"
"$STAGE/test-tls-io"
