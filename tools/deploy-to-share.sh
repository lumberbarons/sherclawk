#!/bin/bash
# Publish Sherclawk with its own creator/bundle flag and the exact validated
# netatalk resource-fork layout. Never restart AFP during a guest session.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
# Required: the AFP server (ssh target) that carries the share. There is no
# default so a published tree never leaks a developer hostname.
SHARE_HOST="${SHARE_HOST:?set SHARE_HOST to the AFP server hostname}"
SHARE_DIR="${SHARE_DIR:-/srv/retro68}"
APP="${APP:-Sherclawk}"
BUILD_TARGET="${BUILD_TARGET:-${APP}_APPL}" "$HERE/build.sh"
python3 "$HERE/tools/netatalk_meta.py" sidecar "$HERE/build" "$APP" "$HERE/build/._$APP"
scp -q "$HERE/build/$APP.APPL" "$SHARE_HOST:/tmp/$APP"
scp -q "$HERE/build/._$APP" "$SHARE_HOST:/tmp/._$APP"
scp -q "$HERE/tools/netatalk_meta.py" "$SHARE_HOST:/tmp/sherclawk-meta.py"
ssh "$SHARE_HOST" "sudo -n install -o macos9 -g macos9 -m 666 /tmp/$APP '$SHARE_DIR/$APP' && sudo -n install -o macos9 -g macos9 -m 666 /tmp/._$APP '$SHARE_DIR/._$APP' && sudo -n python3 /tmp/sherclawk-meta.py xattr '$SHARE_DIR/$APP'"
echo "Published $APP to $SHARE_HOST:$SHARE_DIR"
