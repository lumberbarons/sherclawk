#!/bin/bash
# Host coverage gate: LLVM source-based line and branch coverage over the
# suites built by tools/check.sh. Requires clang plus matching
# llvm-profdata/llvm-cov: macOS Command Line Tools, or Ubuntu clang-18 and
# llvm-18. See docs/development.md#host-checks.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="$HERE/build/coverage"
PROFILE_DIR="$OUT_DIR/profiles"
LCOV_DIR="$OUT_DIR/lcov"
# Floors sit below the recorded baseline (93.6% lines / 69.2% branches over
# non-test sources, stable across Apple clang 21 and Ubuntu clang 18) with
# headroom for new guest-only work; raise deliberately.
MIN_LINES=92
MIN_BRANCHES=67

find_llvm_tool() {
    local name="$1" directory
    if command -v "$name" >/dev/null 2>&1; then
        command -v "$name"
        return 0
    fi
    # Debian/Ubuntu keep versioned LLVM tools outside PATH.
    for directory in /usr/lib/llvm-*/bin; do
        if [ -x "$directory/$name" ]; then
            printf '%s\n' "$directory/$name"
            return 0
        fi
    done
    if command -v xcrun >/dev/null 2>&1 &&
        directory="$(xcrun -f "$name" 2>/dev/null)"; then
        printf '%s\n' "$directory"
        return 0
    fi
    return 1
}

# Prefer one directory that provides clang and both LLVM tools, newest first,
# so profraw and profdata can never come from mismatched releases.
find_llvm_dir() {
    local directory
    # shellcheck disable=SC2046  # word splitting is intended for the glob
    for directory in $(printf '%s\n' /usr/lib/llvm-*/bin | sort -V -r); do
        if [ -x "$directory/clang" ] && [ -x "$directory/llvm-cov" ] &&
            [ -x "$directory/llvm-profdata" ]; then
            printf '%s\n' "$directory"
            return 0
        fi
    done
    return 1
}

LLVM_DIR="$(find_llvm_dir || true)"
if [ -n "$LLVM_DIR" ]; then
    CLANG="$LLVM_DIR/clang"
    LLVM_COV="$LLVM_DIR/llvm-cov"
    LLVM_PROFDATA="$LLVM_DIR/llvm-profdata"
else
    LLVM_COV="$(find_llvm_tool llvm-cov)" || {
        echo "error: llvm-cov not found; install clang and LLVM tools (Ubuntu: apt install clang-18 llvm-18)" >&2
        exit 1
    }
    TOOL_DIR="$(dirname "$LLVM_COV")"
    if [ -x "$TOOL_DIR/llvm-profdata" ]; then
        LLVM_PROFDATA="$TOOL_DIR/llvm-profdata"
    else
        LLVM_PROFDATA="$(find_llvm_tool llvm-profdata)" || {
            echo "error: llvm-profdata not found; install clang and LLVM tools (Ubuntu: apt install clang-18 llvm-18)" >&2
            exit 1
        }
    fi
    CLANG=
    XCRUN_CLANG=
    if command -v xcrun >/dev/null 2>&1; then
        XCRUN_CLANG="$(xcrun -f clang 2>/dev/null || true)"
    fi
    for candidate in "$TOOL_DIR/clang" \
        "$(command -v clang 2>/dev/null || true)" \
        "$XCRUN_CLANG"; do
        if [ -n "$candidate" ] && [ -x "$candidate" ] &&
            "$candidate" --version 2>/dev/null | grep -q clang; then
            CLANG="$candidate"
            break
        fi
    done
    if [ -z "$CLANG" ]; then
        echo "error: no clang found next to $LLVM_COV" >&2
        exit 1
    fi
fi
# A directly invoked Command Line Tools clang needs an explicit sysroot for
# system headers; the /usr/bin/cc shim normally supplies it.
if [ -z "${SDKROOT:-}" ] && command -v xcrun >/dev/null 2>&1; then
    SDKROOT="$(xcrun --show-sdk-path 2>/dev/null || true)"
    if [ -n "$SDKROOT" ]; then
        export SDKROOT
    fi
fi

echo "coverage: $("$CLANG" --version | head -1)"
echo "coverage: $("$LLVM_COV" --version | head -1)"
rm -rf "$PROFILE_DIR" "$LCOV_DIR"
mkdir -p "$PROFILE_DIR" "$LCOV_DIR"

CHECK_COVERAGE=1 CHECK_COVERAGE_DIR="$PROFILE_DIR" CC="$CLANG" \
    bash "$HERE/tools/check.sh"

for profile in "$PROFILE_DIR"/*.profraw; do
    suite="$(basename "$profile" .profraw)"
    "$LLVM_PROFDATA" merge -sparse "$profile" -o "$PROFILE_DIR/$suite.profdata"
    "$LLVM_COV" export "$HERE/build/tests/$suite" \
        -instr-profile="$PROFILE_DIR/$suite.profdata" -format=lcov \
        > "$LCOV_DIR/$suite.lcov"
done

python3 "$HERE/tools/coverage_report.py" --lcov-dir "$LCOV_DIR" \
    --min-lines "$MIN_LINES" --min-branches "$MIN_BRANCHES"
