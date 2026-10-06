#!/bin/bash
# Reuse HelloHTTPS staging so both apps ship the same Certainly correctness
# patches and Universal Interfaces without modifying the upstream checkout.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
python3 "$HERE/tools/embed-project-template.py"
python3 "$HERE/tools/make-art.py" "${SHERCLAWK_ART:-$HERE/../sherclawk.png}" "$HERE/build/art.r"
APP_DIR="$HERE" BUILD_TARGET="${1:-${BUILD_TARGET:-Sherclawk_APPL}}" \
    "$HERE/../hello-https/build.sh"
