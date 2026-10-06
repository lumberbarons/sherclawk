#!/bin/bash
# Protocol and File Manager fault checks run under ASan/UBSan; no VM/key.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$HERE/build/tests"
"${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$HERE" -I"$HERE/../hello-https" \
    "$HERE/tests/test_core.c" "$HERE/chat.c" "$HERE/json.c" "$HERE/text.c" \
    "$HERE/../hello-https/http.c" -o "$HERE/build/tests/test-core"
"$HERE/build/tests/test-core"
"${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$HERE" -I"$HERE/../hello-https" -I"$HERE/../Certainly/include" \
    -I"$HERE/../hello-https/tools/host-tls/shim" \
    "$HERE/tests/test_network.c" "$HERE/network.c" "$HERE/../hello-https/http.c" \
    -o "$HERE/build/tests/test-network"
"$HERE/build/tests/test-network"

"${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I"$HERE" \
    "$HERE/tests/test_agent.c" "$HERE/agent.c" "$HERE/json.c" "$HERE/text.c" \
    -o "$HERE/build/tests/test-agent"
"$HERE/build/tests/test-agent"

"${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror -Wno-multichar \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$HERE/tests/toolbox" -I"$HERE" \
    "$HERE/tests/test_tools.c" "$HERE/tools.c" "$HERE/json.c" "$HERE/text.c" \
    -o "$HERE/build/tests/test-tools"
"$HERE/build/tests/test-tools"

"${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror -Wno-multichar \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$HERE/tests/toolbox" -I"$HERE" \
    "$HERE/tests/test_jobs.c" "$HERE/jobs.c" -o "$HERE/build/tests/test-jobs"
"$HERE/build/tests/test-jobs"
