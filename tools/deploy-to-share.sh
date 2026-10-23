#!/bin/bash
# Publish a built Sherclawk application with its creator/bundle flag and the
# exact validated netatalk resource-fork/xattr layout, through the harness
# publisher. Never restart AFP during a guest session.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
# Required: the AFP server (ssh target) that carries the share. There is no
# default so a published tree never leaks a developer hostname.
SHARE_HOST="${SHARE_HOST:?set SHARE_HOST to the AFP server hostname}"
SHARE_DIR="${SHARE_DIR:-/srv/retro68}"
APP="${APP:-Sherclawk}"
BUILD_TARGET="${BUILD_TARGET:-${APP}_APPL}" "$HERE/build.sh"
python3 "$HERE/harness/publish.py" install \
    --build "$HERE/build" --target "$APP" \
    --share-host "$SHARE_HOST" --share-dir "$SHARE_DIR" \
    --dest "$SHARE_DIR/$APP"
echo "Published $APP to $SHARE_HOST:$SHARE_DIR"
