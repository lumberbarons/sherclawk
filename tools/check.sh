#!/bin/bash
# Protocol and File Manager fault checks run under ASan/UBSan; no VM/key.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
python3 "$HERE/tools/embed-project-template.py"
mkdir -p "$HERE/build/tests"
# Match the app build: SHERCLAWK_APP selects the User-Agent asserted by the test.
"${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -DSHERCLAWK_APP=1 \
    -I"$HERE" -I"$HERE/vendor" \
    "$HERE/tests/test_core.c" "$HERE/chat.c" "$HERE/json.c" "$HERE/text.c" \
    "$HERE/vendor/http.c" -o "$HERE/build/tests/test-core"
"$HERE/build/tests/test-core"

# The network test needs a Certainly clone (fetch-only, see README).
CERTAINLY_DIR="${CERTAINLY_DIR:-$HERE/../Certainly}"
if [ -d "$CERTAINLY_DIR/include" ]; then
    "${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I"$HERE" -I"$HERE/vendor" -I"$CERTAINLY_DIR/include" \
        -I"$HERE/vendor/host-tls/shim" \
        "$HERE/tests/test_network.c" "$HERE/network.c" "$HERE/vendor/http.c" \
        -o "$HERE/build/tests/test-network"
    "$HERE/build/tests/test-network"
else
    echo "skip: network test needs a Certainly clone at $CERTAINLY_DIR" >&2
    echo "      git clone --recursive --depth 1 https://github.com/minorbug/certainly.git Certainly" >&2
fi

"${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I"$HERE" \
    "$HERE/tests/test_agent.c" "$HERE/agent.c" "$HERE/json.c" "$HERE/text.c" \
    -o "$HERE/build/tests/test-agent"
"$HERE/build/tests/test-agent"

"${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror -Wno-multichar \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$HERE/tests/toolbox" -I"$HERE" \
    "$HERE/tests/test_tools.c" "$HERE/tools.c" "$HERE/inspect.c" "$HERE/json.c" "$HERE/text.c" \
    -o "$HERE/build/tests/test-tools"
"$HERE/build/tests/test-tools"

"${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror -Wno-multichar \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$HERE/tests/toolbox" -I"$HERE" \
    "$HERE/tests/test_jobs.c" "$HERE/jobs.c" -o "$HERE/build/tests/test-jobs"
"$HERE/build/tests/test-jobs"

"${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror -Wno-multichar \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$HERE/tests/toolbox" -I"$HERE" \
    "$HERE/tests/test_build_project.c" "$HERE/selfbuild.c" "$HERE/build_project.c" "$HERE/run_application.c" "$HERE/jobs.c" \
    "$HERE/tools.c" "$HERE/inspect.c" "$HERE/json.c" "$HERE/text.c" -o "$HERE/build/tests/test-build-project"
"$HERE/build/tests/test-build-project"

"${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror -Wno-multichar \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$HERE/tests/toolbox" -I"$HERE" \
    "$HERE/tests/test_selfbuild.c" "$HERE/selfbuild.c" "$HERE/build_project.c" \
    "$HERE/run_application.c" "$HERE/jobs.c" "$HERE/tools.c" "$HERE/inspect.c" "$HERE/json.c" "$HERE/text.c" \
    -o "$HERE/build/tests/test-selfbuild"
"$HERE/build/tests/test-selfbuild"
