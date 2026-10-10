#!/usr/bin/env python3
"""Aggregate the per-suite lcov exports produced by tools/check-coverage.sh.

Each host suite is a separate binary with its own profile, so coverage is the
union over suites: a line, function or branch counts as covered when at least
one suite exercises it. Only repository sources the host suites compile are
gated (tests and staged/build inputs are skipped), and missing branch data is
an error rather than a silent fall back to line-only coverage.
"""
import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lcov-dir', type=Path, required=True,
                        help='directory of per-suite lcov exports')
    parser.add_argument('--min-lines', type=float, default=92.0,
                        help='minimum gated line coverage percent (default: 92)')
    parser.add_argument('--min-branches', type=float, default=67.0,
                        help='minimum gated branch coverage percent (default: 67)')
    return parser.parse_args()


def gated_path(source_file, root):
    """Resolve an lcov source file to a gated repository path, or None."""
    path = Path(source_file)
    if not path.is_absolute():
        path = root / path
    try:
        relative = path.relative_to(root)
    except ValueError:
        return None
    if relative.parts and relative.parts[0] in ('build', 'tests'):
        return None
    return str(relative)


def record(store, path, key, value):
    previous = store.setdefault(path, {}).get(key)
    if previous is None or value > previous:
        store[path][key] = value


def parse_lcov(directory, root):
    lines = {}
    functions = {}
    branches = {}
    found = False
    for lcov in sorted(directory.glob('*.lcov')):
        found = True
        path = None
        for row in lcov.read_text(errors='replace').splitlines():
            if row.startswith('SF:'):
                path = gated_path(row[3:], root)
            elif path is None:
                continue
            elif row.startswith('DA:'):
                line, count = row[3:].split(',')[:2]
                record(lines, path, int(line), int(count))
            elif row.startswith('FNDA:'):
                count, name = row[5:].split(',', 1)
                record(functions, path, name, int(count))
            elif row.startswith('BRDA:'):
                line, block, branch, taken = row[5:].split(',')
                record(branches, path, (int(line), block, branch),
                       0 if taken == '-' else int(taken))
    return found, lines, functions, branches


def totals(store):
    total = sum(len(entries) for entries in store.values())
    hit = sum(1 for entries in store.values()
              for count in entries.values() if count > 0)
    return hit, total


def percent(hit, total):
    return 100.0 * hit / total if total else 0.0


def main():
    args = parse_args()
    found, lines, functions, branches = parse_lcov(args.lcov_dir, REPO_ROOT)
    if not found:
        print(f'error: no lcov exports in {args.lcov_dir}', file=sys.stderr)
        return 2
    if not lines:
        print('error: no gated repository sources in the coverage exports',
              file=sys.stderr)
        return 2
    branch_hit, branch_total = totals(branches)
    if not branch_total:
        print('error: no branch data; the compiler emitted no branch regions '
              '(use clang with -fprofile-instr-generate -fcoverage-mapping)',
              file=sys.stderr)
        return 2

    width = max(len('file'), *(len(path) for path in lines))
    print(f'{"file":<{width}} {"lines":>13} {"branches":>14} {"functions":>14}')

    def branch_weakness(path):
        hit, total = totals({path: branches.get(path, {})})
        return percent(hit, total) if total else 101.0

    def line_percent(path):
        hit, total = totals({path: lines.get(path, {})})
        return percent(hit, total)

    for path in sorted(lines, key=lambda p: (branch_weakness(p), line_percent(p))):
        line_hit, line_total = totals({path: lines[path]})
        func_hit, func_total = totals({path: functions.get(path, {})})
        br_hit, br_total = totals({path: branches.get(path, {})})
        line_cell = f'{line_hit}/{line_total} {percent(line_hit, line_total):5.1f}%'
        branch_cell = (f'{br_hit}/{br_total} {percent(br_hit, br_total):5.1f}%'
                       if br_total else 'none')
        func_cell = f'{func_hit}/{func_total} {percent(func_hit, func_total):5.1f}%'
        print(f'{path:<{width}} {line_cell:>13} {branch_cell:>14} {func_cell:>14}')

    line_hit, line_total = totals(lines)
    func_hit, func_total = totals(functions)
    line_pct = percent(line_hit, line_total)
    branch_pct = percent(branch_hit, branch_total)
    print()
    print(f'gated sources: {len(lines)} files '
          '(repository sources compiled by the host suites; tests excluded)')
    print(f'totals: lines {line_hit}/{line_total} ({line_pct:.1f}%), '
          f'branches {branch_hit}/{branch_total} ({branch_pct:.1f}%), '
          f'functions {func_hit}/{func_total} ({percent(func_hit, func_total):.1f}%)')
    print(f'minimum: {args.min_lines:.1f}% lines, {args.min_branches:.1f}% branches')

    failures = []
    if line_pct < args.min_lines:
        failures.append(f'lines {line_pct:.1f}% < {args.min_lines:.1f}%')
    if branch_pct < args.min_branches:
        failures.append(f'branches {branch_pct:.1f}% < {args.min_branches:.1f}%')
    if failures:
        print('FAIL: ' + '; '.join(failures) + ' (weakest files listed first)')
        return 1
    print('PASS: line and branch coverage meet the minimums')
    return 0


if __name__ == '__main__':
    sys.exit(main())
