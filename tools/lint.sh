#!/bin/bash
# Static analysis shared by developers and CI: shellcheck for the shell
# tools, cppcheck for the C sources the host tests compile, and ruff (rules
# in ruff.toml) for the Python helpers. A missing tool is skipped with a
# note; CI installs all three before running this script.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"
status=0

if command -v shellcheck >/dev/null 2>&1; then
    echo "shellcheck: shell tools"
    shellcheck build.sh tools/*.sh worker/*.sh templates/ppc-toolbox/*.sh || status=1
else
    echo "skip: shellcheck not installed" >&2
fi

if command -v cppcheck >/dev/null 2>&1; then
    echo "cppcheck: shared sources and host tests"
    # The host tests never define NDEBUG; their fixture clock and journal
    # spies advance inside assert() on purpose, so scope those two checkers
    # to the test files where the pattern lives (cppcheck 2.13 on CI emits
    # both; newer cppcheck only the first).
    cppcheck --enable=warning,performance,portability --error-exitcode=1 \
        --inline-suppr --std=c99 --suppress=missingIncludeSystem \
        --suppress=assignmentInAssert:tests/test_tools.c \
        --suppress=assignmentInAssert:tests/test_selfbuild.c \
        --suppress=assertWithSideEffect:tests/test_jobs.c \
        --suppress=assertWithSideEffect:tests/test_build_project.c \
        -I tests/toolbox -I . -I vendor \
        tools.c inspect.c json.c text.c chat.c agent.c network.c jobs.c \
        selfbuild.c build_project.c run_application.c session.c vendor/http.c \
        tests/test_core.c tests/test_tools.c tests/test_agent.c \
        tests/test_jobs.c tests/test_build_project.c tests/test_selfbuild.c \
        tests/test_session.c \
        || status=1
else
    echo "skip: cppcheck not installed" >&2
fi

if command -v ruff >/dev/null 2>&1; then
    echo "ruff: Python tools"
    ruff check tools tests || status=1
else
    echo "skip: ruff not installed (pip install ruff)" >&2
fi

exit "$status"
