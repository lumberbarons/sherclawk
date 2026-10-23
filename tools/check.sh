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
# guest-verified limit until src/agent/text_limits.h records acceptance.
WARN=(-Wall -Wextra -Werror -DSHERCLAWK_LARGE_TEXT_CHECK=1)
if ! "${CC:-cc}" -dM -E -x c /dev/null 2>/dev/null | grep -q __clang__; then
    WARN+=(-Wno-format-truncation)
fi
SAN=("-fsanitize=address,undefined" -fno-omit-frame-pointer)
# Suites append their deterministic cost totals (tests/cost.h) here; the table
# prints at the end and every total is also an upper-bound assertion.
export SHERCLAWK_COST_LOG="$HERE/build/cost.log"
: > "$SHERCLAWK_COST_LOG"
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
    -I"$HERE" -I"$HERE/src" -I"$HERE/vendor" \
    "$HERE/tests/test_core.c" "$HERE/src/app/chat.c" "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/text.c" \
    "$HERE/vendor/http.c"
run_test test-core

# Shared bounded argument/result helpers are pure C.
build_test test-jsonargs "${WARN[@]}" -I"$HERE" -I"$HERE/src" \
    "$HERE/tests/test_jsonargs.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/json.c"
run_test test-jsonargs

build_test test-display "${WARN[@]}" -I"$HERE" -I"$HERE/src" \
    "$HERE/tests/test_display.c" "$HERE/src/app/display.c" "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c"
run_test test-display

build_test test-chat-display "${WARN[@]}" -I"$HERE" -I"$HERE/src" \
    "$HERE/tests/test_chat_display.c" "$HERE/src/app/chat.c" "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/text.c"
run_test test-chat-display

build_test test-timing -Wall -Wextra -Werror -I"$HERE" -I"$HERE/src" \
    "$HERE/tests/test_timing.c" "$HERE/src/app/timing.c"
run_test test-timing

# Preferences parser/formatter: pure code, no Toolbox stubs needed.
build_test test-preferences -Wall -Wextra -Werror -DSHERCLAWK_APP=1 -I"$HERE" -I"$HERE/src" \
    "$HERE/tests/test_preferences.c" "$HERE/src/app/preferences.c"
run_test test-preferences

# Preferences model picker: filtering, fetch transitions and confirm against a
# fake host, with the real catalog page parser.
build_test test-prefs-model "${WARN[@]}" -I"$HERE" -I"$HERE/src" -I"$HERE/tests/toolbox" \
    "$HERE/tests/test_prefs_model.c" "$HERE/src/app/prefs_model.c" "$HERE/src/agent/agent.c" "$HERE/src/agent/registry.c" \
    "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/text.c"
run_test test-prefs-model

# Application run controller: production protocol with fake platform effects.
build_test test-run-ctl "${WARN[@]}" -DSHERCLAWK_HOST=1 \
    -I"$HERE" -I"$HERE/src" -I"$HERE/vendor" -I"$HERE/tests/run_ctl" -I"$HERE/tests/toolbox" \
    "$HERE/tests/test_run_ctl.c" "$HERE/src/app/run_ctl.c" "$HERE/src/app/display.c" \
    "$HERE/src/app/preferences.c" "$HERE/src/app/timing.c" "$HERE/src/agent/agent.c" \
    "$HERE/src/agent/registry.c" "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" \
    "$HERE/src/agent/text.c" "$HERE/vendor/http.c"
run_test test-run-ctl

# MCP pure configuration/protocol/streaming fixtures, independent of keys.
build_test test-mcp "${WARN[@]}" -I"$HERE" -I"$HERE/src" -I"$HERE/vendor" \
    "$HERE/tests/test_mcp.c" "$HERE/src/mcp/config.c" "$HERE/src/mcp/protocol.c" \
    "$HERE/src/mcp/stream.c" "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/text.c" "$HERE/vendor/http.c"
run_test test-mcp

# Resumable MCP parsing, key checks, redaction and discovery against the
# synchronous implementations they replaced, with per-step work bounds.
build_test test-mcp-work "${WARN[@]}" -I"$HERE" -I"$HERE/src" -I"$HERE/vendor" \
    "$HERE/tests/test_mcp_work.c" "$HERE/src/mcp/config.c" "$HERE/src/mcp/protocol.c" \
    "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/text.c" "$HERE/vendor/http.c"
run_test test-mcp-work

# Credential redaction (decoded and JSON-escaped reflections) in schemas, names
# and call results, against an independent reference.
build_test test-mcp-redact "${WARN[@]}" -I"$HERE" -I"$HERE/src" -I"$HERE/vendor" \
    "$HERE/tests/test_mcp_redact.c" "$HERE/src/mcp/config.c" "$HERE/src/mcp/protocol.c" \
    "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/text.c" "$HERE/vendor/http.c"
run_test test-mcp-redact

# MCP editor text rules and the staged-replace store (File Manager fault model).
"${CC:-cc}" -std=c99 -g -O1 "${WARN[@]}" \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$HERE" -I"$HERE/src" -I"$HERE/vendor" \
    "$HERE/tests/test_mcp_editor.c" "$HERE/src/mcp/editor.c" "$HERE/src/mcp/config.c" \
    "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/text.c" "$HERE/vendor/http.c" \
    -o "$HERE/build/tests/test-mcp-editor"
"$HERE/build/tests/test-mcp-editor"
"${CC:-cc}" -std=c99 -g -O1 "${WARN[@]}" -Wno-multichar \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$HERE" -I"$HERE/src" -I"$HERE/tests/toolbox" \
    "$HERE/tests/test_mcp_store.c" "$HERE/src/mcp/store.c" \
    -o "$HERE/build/tests/test-mcp-store"
"$HERE/build/tests/test-mcp-store"

# The network test needs a Certainly clone (fetch-only, see README); checks
# run against the commit pinned in tools/certainly.sh.
. "$HERE/tools/certainly.sh"
CERTAINLY_DIR="$(certainly_dir)"
if [ -d "$CERTAINLY_DIR" ]; then
    certainly_ensure "$CERTAINLY_DIR"
    build_test test-network "${WARN[@]}" \
        -I"$HERE" -I"$HERE/src" -I"$HERE/vendor" -I"$CERTAINLY_DIR/include" \
        -I"$HERE/vendor/host-tls/shim" \
        "$HERE/tests/test_network.c" "$HERE/src/agent/network.c" "$HERE/vendor/http.c"
    run_test test-network
    build_test test-mcp-client "${WARN[@]}" \
        -I"$HERE" -I"$HERE/src" -I"$HERE/vendor" -I"$CERTAINLY_DIR/include" \
        -I"$HERE/vendor/host-tls/shim" \
        "$HERE/tests/test_mcp_client.c" "$HERE/src/mcp/client.c" \
        "$HERE/src/mcp/config.c" "$HERE/src/mcp/protocol.c" "$HERE/src/mcp/stream.c" \
        "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/vendor/http.c"
    run_test test-mcp-client
    # The run controller src/app/main.c drives (src/mcp/run.c): configuration loading,
    # discovery, the schema freeze, sequential dispatch, retention, paging,
    # Stop, the drain and the native-only fallbacks, with the File Manager
    # model in its own unit (the Toolbox headers and the host TLS shim both
    # define noErr).
    build_test test-mcp-run "${WARN[@]}" -Wno-multichar \
        -I"$HERE" -I"$HERE/src" -I"$HERE/vendor" -I"$CERTAINLY_DIR/include" \
        -I"$HERE/vendor/host-tls/shim" -I"$HERE/tests/toolbox" \
        "$HERE/tests/test_mcp_run.c" "$HERE/tests/test_mcp_run_fm.c" \
        "$HERE/src/mcp/run.c" "$HERE/src/mcp/client.c" "$HERE/src/mcp/config.c" \
        "$HERE/src/mcp/protocol.c" "$HERE/src/mcp/stream.c" "$HERE/src/mcp/result.c" "$HERE/src/agent/agent.c" \
        "$HERE/src/agent/registry.c" "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/text.c" "$HERE/vendor/http.c"
    run_test test-mcp-run
else
    echo "skip: network test needs a Certainly clone at $CERTAINLY_DIR" >&2
    echo "      git clone --depth 1 $CERTAINLY_REPO Certainly" >&2
    echo "      (all checks use the commit pinned in tools/certainly.sh)" >&2
fi

build_test test-base64 "${WARN[@]}" -I"$HERE" -I"$HERE/src" \
    "$HERE/tests/test_base64.c"
run_test test-base64

build_test test-agent "${WARN[@]}" -I"$HERE" -I"$HERE/src" -I"$HERE/tests/toolbox" \
    "$HERE/tests/test_agent.c" "$HERE/src/agent/agent.c" "$HERE/src/agent/registry.c" "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/text.c"
run_test test-agent

build_test test-tools "${WARN[@]}" -Wno-multichar \
    -I"$HERE/tests/toolbox" -I"$HERE" -I"$HERE/src" -I"$HERE/vendor" \
    "$HERE/tests/test_tools.c" "$HERE/src/tools/tools.c" "$HERE/src/tools/fsutil.c" "$HERE/src/agent/registry.c" "$HERE/src/agent/agent.c" \
    "$HERE/src/mcp/guard.c" "$HERE/src/mcp/result.c" "$HERE/src/mcp/config.c" "$HERE/src/tools/inspect.c" "$HERE/src/tools/view_image.c" \
    "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/text.c" "$HERE/vendor/http.c"
run_test test-tools

# The sealed queue guard must follow configuration, including its length.
build_test test-tools-queue "${WARN[@]}" -Wno-multichar \
    -DSHERCLAWK_BUILD_QUEUE='"Build Service:pending"' -DSHERCLAWK_QUEUE_GUARD_CHECK=1 \
    -I"$HERE/tests/toolbox" -I"$HERE" -I"$HERE/src" -I"$HERE/vendor" \
    "$HERE/tests/test_tools.c" "$HERE/src/tools/tools.c" "$HERE/src/tools/fsutil.c" "$HERE/src/agent/registry.c" "$HERE/src/agent/agent.c" \
    "$HERE/src/mcp/guard.c" "$HERE/src/mcp/result.c" "$HERE/src/mcp/config.c" "$HERE/src/tools/inspect.c" "$HERE/src/tools/view_image.c" \
    "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/text.c" "$HERE/vendor/http.c"
run_test test-tools-queue

build_test test-mcp-result "${WARN[@]}" -Wno-multichar \
    -I"$HERE/tests/toolbox" -I"$HERE" -I"$HERE/src" -I"$HERE/vendor" \
    "$HERE/tests/test_mcp_result.c" "$HERE/src/mcp/result.c" "$HERE/src/mcp/config.c" \
    "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/text.c" "$HERE/vendor/http.c"
run_test test-mcp-result

build_test test-text-gate "${WARN[@]}" -USHERCLAWK_LARGE_TEXT_CHECK -Wno-multichar \
    -I"$HERE/tests/toolbox" -I"$HERE" -I"$HERE/src" -I"$HERE/vendor" \
    "$HERE/tests/test_text_gate.c" "$HERE/src/tools/tools.c" "$HERE/src/tools/fsutil.c" "$HERE/src/agent/registry.c" "$HERE/src/agent/agent.c" \
    "$HERE/src/mcp/guard.c" "$HERE/src/mcp/result.c" "$HERE/src/mcp/config.c" "$HERE/src/tools/inspect.c" "$HERE/src/tools/view_image.c" \
    "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/text.c" "$HERE/vendor/http.c"
run_test test-text-gate

build_test test-fsutil "${WARN[@]}" -Wno-multichar \
    -I"$HERE/tests/toolbox" -I"$HERE" -I"$HERE/src" \
    "$HERE/tests/test_fsutil.c" "$HERE/src/tools/fsutil.c"
run_test test-fsutil

build_test test-jobs "${WARN[@]}" -Wno-multichar \
    -I"$HERE/tests/toolbox" -I"$HERE" -I"$HERE/src" \
    "$HERE/tests/test_jobs.c" "$HERE/src/native/jobs.c" "$HERE/src/tools/fsutil.c"
run_test test-jobs

build_test test-build-project "${WARN[@]}" -Wno-multichar \
    -I"$HERE/tests/toolbox" -I"$HERE" -I"$HERE/src" -I"$HERE/vendor" \
    "$HERE/tests/test_build_project.c" "$HERE/src/native/selfbuild.c" "$HERE/src/native/build_project.c" \
    "$HERE/src/native/run_application.c" "$HERE/src/native/application_process.c" "$HERE/src/native/ae_dispatch.c" \
    "$HERE/src/native/jobs.c" "$HERE/src/tools/tools.c" "$HERE/src/tools/fsutil.c" "$HERE/src/agent/registry.c" "$HERE/src/agent/agent.c" "$HERE/src/mcp/guard.c" "$HERE/src/mcp/result.c" \
    "$HERE/src/mcp/config.c" "$HERE/src/tools/inspect.c" "$HERE/src/tools/view_image.c" \
    "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/text.c" "$HERE/vendor/http.c"
run_test test-build-project

build_test test-selfbuild "${WARN[@]}" -Wno-multichar \
    -I"$HERE/tests/toolbox" -I"$HERE" -I"$HERE/src" -I"$HERE/vendor" \
    "$HERE/tests/test_selfbuild.c" "$HERE/src/native/selfbuild.c" "$HERE/src/native/build_project.c" \
    "$HERE/src/native/run_application.c" "$HERE/src/native/application_process.c" "$HERE/src/native/ae_dispatch.c" \
    "$HERE/src/native/jobs.c" "$HERE/src/tools/tools.c" "$HERE/src/tools/fsutil.c" "$HERE/src/agent/registry.c" "$HERE/src/agent/agent.c" "$HERE/src/mcp/guard.c" "$HERE/src/mcp/result.c" \
    "$HERE/src/mcp/config.c" "$HERE/src/tools/inspect.c" "$HERE/src/tools/view_image.c" \
    "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/text.c" "$HERE/vendor/http.c"
run_test test-selfbuild

build_test test-quit-application "${WARN[@]}" -Wno-multichar \
    -I"$HERE/tests/toolbox" -I"$HERE" -I"$HERE/src" -I"$HERE/vendor" \
    "$HERE/tests/test_quit_application.c" "$HERE/src/native/selfbuild.c" "$HERE/src/native/build_project.c" \
    "$HERE/src/native/run_application.c" "$HERE/src/native/application_process.c" "$HERE/src/native/ae_dispatch.c" \
    "$HERE/src/native/toolserver.c" "$HERE/src/native/jobs.c" "$HERE/src/tools/tools.c" "$HERE/src/tools/fsutil.c" "$HERE/src/agent/registry.c" "$HERE/src/agent/agent.c" "$HERE/src/mcp/guard.c" \
    "$HERE/src/mcp/result.c" "$HERE/src/mcp/config.c" "$HERE/src/tools/inspect.c" \
    "$HERE/src/tools/view_image.c" "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/text.c" "$HERE/vendor/http.c"
run_test test-quit-application

build_test test-session "${WARN[@]}" -Wno-multichar \
    -I"$HERE/tests/toolbox" -I"$HERE" -I"$HERE/src" -I"$HERE/vendor" \
    "$HERE/tests/test_session.c" "$HERE/src/app/session.c" "$HERE/src/mcp/result.c" "$HERE/src/mcp/config.c" \
    "$HERE/src/agent/agent.c" "$HERE/src/agent/registry.c" \
    "$HERE/src/agent/json.c" "$HERE/src/agent/jsonargs.c" "$HERE/src/agent/text.c" "$HERE/vendor/http.c"
run_test test-session

# The starter's file output and self-render run against a modelled Toolbox
# (tests/template); the scene, controls and real GWorld are guest-only.
build_test test-template "${WARN[@]}" -Wno-multichar -Wno-deprecated-declarations \
    -I"$HERE/tests/template" -I"$HERE/templates/ppc-toolbox" \
    "$HERE/tests/template/test_template.c" "$HERE/templates/ppc-toolbox/io.c" \
    "$HERE/templates/ppc-toolbox/png.c" "$HERE/templates/ppc-toolbox/selfrender.c"
run_test test-template

# The OS 9 guest harness (ADR-0009) keeps its own stdlib-only suites; they
# need no QEMU, VM or network.
build_test test-harness-contract "${WARN[@]}" -I"$HERE" \
    "$HERE/harness/tests/test_contract.c" "$HERE/harness/native/contract.c"
run_test test-harness-contract
build_test test-harness-run "${WARN[@]}" -I"$HERE" \
    "$HERE/harness/tests/test_run.c" "$HERE/harness/native/run.c" "$HERE/harness/native/contract.c"
run_test test-harness-run
python3 -I -m unittest discover -s "$HERE/harness/tests"

echo
echo "Cost table (host counts; convert to guest ticks with docs/development.md#cost-table-and-guest-calibration)"
printf '%-62s %8s %8s\n' counter measured bound
cat "$SHERCLAWK_COST_LOG"
