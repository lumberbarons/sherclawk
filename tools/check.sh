#!/bin/bash
# Protocol and File Manager fault checks run under ASan/UBSan; no VM/key.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
python3 "$HERE/tools/embed-project-template.py"
mkdir -p "$HERE/build/tests"
# The host checks also run under GCC (the Linux CI runner): GCC warns about
# intentional bounded-snprintf truncation into fixed-size Mac buffers, which
# clang does not. Keep -Werror for every other diagnostic.
# Exercise the not-yet-advertised large-file path; shipping builds keep the
# guest-verified limit until text_limits.h records acceptance.
WARN=(-Wall -Wextra -Werror -DSHERCLAWK_LARGE_TEXT_CHECK=1)
if ! "${CC:-cc}" -dM -E -x c /dev/null 2>/dev/null | grep -q __clang__; then
    WARN+=(-Wno-format-truncation)
fi
SAN=("-fsanitize=address,undefined" -fno-omit-frame-pointer)
# tools/check-coverage.sh sets CHECK_COVERAGE=1 to build profile-instrumented
# binaries and keep each suite's raw profile for the aggregated report.
COV=()
COV_DIR=
if [ "${CHECK_COVERAGE:-0}" = 1 ]; then
    COV=(-fprofile-instr-generate -fcoverage-mapping)
    COV_DIR="${CHECK_COVERAGE_DIR:-$HERE/build/coverage/profiles}"
    mkdir -p "$COV_DIR"
fi

build_test() {
    local name="$1"
    shift
    if [ -n "$COV_DIR" ]; then
        "${CC:-cc}" -std=c99 -g -O1 "${SAN[@]}" "${COV[@]}" "$@" \
            -o "$HERE/build/tests/$name"
    else
        "${CC:-cc}" -std=c99 -g -O1 "${SAN[@]}" "$@" \
            -o "$HERE/build/tests/$name"
    fi
}

run_test() {
    local name="$1"
    if [ -n "$COV_DIR" ]; then
        LLVM_PROFILE_FILE="$COV_DIR/$name.profraw" "$HERE/build/tests/$name"
    else
        "$HERE/build/tests/$name"
    fi
}

# Match the app build: SHERCLAWK_APP selects the User-Agent asserted by the test.
build_test test-core "${WARN[@]}" -DSHERCLAWK_APP=1 \
    -I"$HERE" -I"$HERE/vendor" \
    "$HERE/tests/test_core.c" "$HERE/chat.c" "$HERE/json.c" "$HERE/text.c" \
    "$HERE/vendor/http.c"
run_test test-core

build_test test-display "${WARN[@]}" -I"$HERE" \
    "$HERE/tests/test_display.c" "$HERE/display.c" "$HERE/json.c"
run_test test-display

build_test test-chat-display "${WARN[@]}" -I"$HERE" \
    "$HERE/tests/test_chat_display.c" "$HERE/chat.c" "$HERE/json.c" "$HERE/text.c"
run_test test-chat-display

build_test test-timing -Wall -Wextra -Werror -I"$HERE" \
    "$HERE/tests/test_timing.c" "$HERE/timing.c"
run_test test-timing

# Preferences parser/formatter: pure code, no Toolbox stubs needed.
build_test test-preferences -Wall -Wextra -Werror -DSHERCLAWK_APP=1 -I"$HERE" \
    "$HERE/tests/test_preferences.c" "$HERE/preferences.c"
run_test test-preferences

# MCP pure configuration/protocol/streaming fixtures, independent of keys.
build_test test-mcp "${WARN[@]}" -I"$HERE" -I"$HERE/vendor" \
    "$HERE/tests/test_mcp.c" "$HERE/mcp_config.c" "$HERE/mcp_protocol.c" \
    "$HERE/mcp_stream.c" "$HERE/json.c" "$HERE/text.c" "$HERE/vendor/http.c"
run_test test-mcp

# Resumable MCP parsing, key checks, redaction and discovery against the
# synchronous implementations they replaced, with per-step work bounds.
build_test test-mcp-work "${WARN[@]}" -I"$HERE" -I"$HERE/vendor" \
    "$HERE/tests/test_mcp_work.c" "$HERE/mcp_config.c" "$HERE/mcp_protocol.c" \
    "$HERE/json.c" "$HERE/text.c" "$HERE/vendor/http.c"
run_test test-mcp-work

# MCP editor text rules and the staged-replace store (File Manager fault model).
"${CC:-cc}" -std=c99 -g -O1 "${WARN[@]}" \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$HERE" -I"$HERE/vendor" \
    "$HERE/tests/test_mcp_editor.c" "$HERE/mcp_editor.c" "$HERE/mcp_config.c" \
    "$HERE/json.c" "$HERE/text.c" "$HERE/vendor/http.c" \
    -o "$HERE/build/tests/test-mcp-editor"
"$HERE/build/tests/test-mcp-editor"
"${CC:-cc}" -std=c99 -g -O1 "${WARN[@]}" -Wno-multichar \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$HERE" -I"$HERE/tests/toolbox" \
    "$HERE/tests/test_mcp_store.c" "$HERE/mcp_store.c" \
    -o "$HERE/build/tests/test-mcp-store"
"$HERE/build/tests/test-mcp-store"

# The network test needs a Certainly clone (fetch-only, see README).
CERTAINLY_DIR="${CERTAINLY_DIR:-$HERE/../Certainly}"
if [ -d "$CERTAINLY_DIR/include" ]; then
    build_test test-network "${WARN[@]}" \
        -I"$HERE" -I"$HERE/vendor" -I"$CERTAINLY_DIR/include" \
        -I"$HERE/vendor/host-tls/shim" \
        "$HERE/tests/test_network.c" "$HERE/network.c" "$HERE/vendor/http.c"
    run_test test-network
    build_test test-mcp-client "${WARN[@]}" \
        -I"$HERE" -I"$HERE/vendor" -I"$CERTAINLY_DIR/include" \
        -I"$HERE/vendor/host-tls/shim" \
        "$HERE/tests/test_mcp_client.c" "$HERE/mcp_client.c" \
        "$HERE/mcp_config.c" "$HERE/mcp_protocol.c" "$HERE/mcp_stream.c" \
        "$HERE/json.c" "$HERE/vendor/http.c"
    run_test test-mcp-client
else
    echo "skip: network test needs a Certainly clone at $CERTAINLY_DIR" >&2
    echo "      git clone --recursive --depth 1 https://github.com/minorbug/certainly.git Certainly" >&2
fi

build_test test-base64 "${WARN[@]}" -I"$HERE" \
    "$HERE/tests/test_base64.c"
run_test test-base64

build_test test-agent "${WARN[@]}" -I"$HERE" -I"$HERE/tests/toolbox" \
    "$HERE/tests/test_agent.c" "$HERE/agent.c" "$HERE/registry.c" "$HERE/json.c" "$HERE/text.c"
run_test test-agent

build_test test-tools "${WARN[@]}" -Wno-multichar \
    -I"$HERE/tests/toolbox" -I"$HERE" \
    "$HERE/tests/test_tools.c" "$HERE/tools.c" "$HERE/registry.c" "$HERE/agent.c" \
    "$HERE/mcp_guard.c" "$HERE/inspect.c" "$HERE/view_image.c" \
    "$HERE/json.c" "$HERE/text.c"
run_test test-tools

build_test test-text-gate "${WARN[@]}" -USHERCLAWK_LARGE_TEXT_CHECK -Wno-multichar \
    -I"$HERE/tests/toolbox" -I"$HERE" \
    "$HERE/tests/test_text_gate.c" "$HERE/tools.c" "$HERE/registry.c" "$HERE/agent.c" \
    "$HERE/mcp_guard.c" "$HERE/inspect.c" "$HERE/view_image.c" \
    "$HERE/json.c" "$HERE/text.c"
run_test test-text-gate

build_test test-jobs "${WARN[@]}" -Wno-multichar \
    -I"$HERE/tests/toolbox" -I"$HERE" \
    "$HERE/tests/test_jobs.c" "$HERE/jobs.c"
run_test test-jobs

build_test test-build-project "${WARN[@]}" -Wno-multichar \
    -I"$HERE/tests/toolbox" -I"$HERE" \
    "$HERE/tests/test_build_project.c" "$HERE/selfbuild.c" "$HERE/build_project.c" \
    "$HERE/run_application.c" "$HERE/application_process.c" "$HERE/ae_dispatch.c" \
    "$HERE/jobs.c" "$HERE/tools.c" "$HERE/registry.c" "$HERE/agent.c" "$HERE/mcp_guard.c" "$HERE/inspect.c" "$HERE/view_image.c" \
    "$HERE/json.c" "$HERE/text.c"
run_test test-build-project

build_test test-selfbuild "${WARN[@]}" -Wno-multichar \
    -I"$HERE/tests/toolbox" -I"$HERE" \
    "$HERE/tests/test_selfbuild.c" "$HERE/selfbuild.c" "$HERE/build_project.c" \
    "$HERE/run_application.c" "$HERE/application_process.c" "$HERE/ae_dispatch.c" \
    "$HERE/jobs.c" "$HERE/tools.c" "$HERE/registry.c" "$HERE/agent.c" "$HERE/mcp_guard.c" "$HERE/inspect.c" "$HERE/view_image.c" \
    "$HERE/json.c" "$HERE/text.c"
run_test test-selfbuild

build_test test-quit-application "${WARN[@]}" -Wno-multichar \
    -I"$HERE/tests/toolbox" -I"$HERE" \
    "$HERE/tests/test_quit_application.c" "$HERE/selfbuild.c" "$HERE/build_project.c" \
    "$HERE/run_application.c" "$HERE/application_process.c" "$HERE/ae_dispatch.c" \
    "$HERE/toolserver.c" "$HERE/jobs.c" "$HERE/tools.c" "$HERE/registry.c" "$HERE/agent.c" "$HERE/mcp_guard.c" "$HERE/inspect.c" \
    "$HERE/view_image.c" "$HERE/json.c" "$HERE/text.c"
run_test test-quit-application

build_test test-session "${WARN[@]}" -Wno-multichar \
    -I"$HERE/tests/toolbox" -I"$HERE" \
    "$HERE/tests/test_session.c" "$HERE/session.c" "$HERE/agent.c" "$HERE/registry.c" \
    "$HERE/json.c" "$HERE/text.c"
run_test test-session

# The starter's file output and self-render run against a modelled Toolbox
# (tests/template); the scene, controls and real GWorld are guest-only.
build_test test-template "${WARN[@]}" -Wno-multichar -Wno-deprecated-declarations \
    -I"$HERE/tests/template" -I"$HERE/templates/ppc-toolbox" \
    "$HERE/tests/template/test_template.c" "$HERE/templates/ppc-toolbox/io.c" \
    "$HERE/templates/ppc-toolbox/png.c" "$HERE/templates/ppc-toolbox/selfrender.c"
run_test test-template
