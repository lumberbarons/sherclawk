#!/usr/bin/env python3
"""Phase timings and throughput from retained suite evidence.

Reads the ``suite.json`` that ``suite.py run`` leaves, plus the per-run
``evidence.json`` it links, and reports where the time went. It only reads
files, so it needs no guest, lease or share and can be rerun on old evidence:

    python3 harness/timing.py report SUITE_DIR [SUITE_DIR ...]

One directory reports that suite's phases. Several directories are treated as
suites that ran at the same time, one per slot, and the report adds the
combined wall time and checks per hour so one-versus-two guest throughput can
be compared from measurements instead of estimated.
"""
import argparse
import json
import os
import sys

SUMMARY_NAME = "suite.json"
# Publication steps recorded by publish.py; "collect" is the wait for the
# guest diagnostic plus reading its closed result.
PUBLISH_PREFIXES = ("application", "fixture:", "request")


class TimingError(Exception):
    pass


def _load(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError) as exc:
        raise TimingError("cannot read %s: %s" % (path, exc))


def _span(start, end):
    return round(end["monotonic"] - start["monotonic"], 3)


def _steps(check):
    """Return (publish, collect) seconds from the run's own step records."""
    evidence = (check.get("run") or {}).get("evidence")
    if not evidence:
        return None, None
    try:
        steps = _load(evidence).get("steps") or []
    except TimingError:
        return None, None
    publish = collect = 0.0
    for step in steps:
        if "finished" not in step:
            continue
        seconds = step["finished"] - step["started"]
        if step["name"] == "collect":
            collect += seconds
        elif step["name"].startswith(PUBLISH_PREFIXES):
            publish += seconds
    return round(publish, 3), round(collect, 3)


def suite_timing(directory):
    summary = _load(os.path.join(directory, SUMMARY_NAME))
    started, finished = summary.get("started"), summary.get("finished")
    if not started or not finished:
        raise TimingError("%s has not finished; no timings to report" % directory)
    checks = summary.get("checks") or []
    reservations = summary.get("reservations") or []
    phases = {"boot": 0.0, "publication": 0.0, "diagnostic": 0.0, "reset": 0.0,
              "release": 0.0, "other": 0.0}
    rows = []
    cursor = started
    for index, check in enumerate(checks):
        # A reservation stamped between two checks is a reset and reboot.
        for reservation in reservations:
            at = reservation["at"]
            if cursor["monotonic"] <= at["monotonic"] <= check["started"]["monotonic"]:
                key = "boot" if reservation is reservations[0] else "reset"
                phases[key] += _span(cursor, at)
                cursor = at
        publish, collect = _steps(check)
        seconds = check.get("seconds") or _span(check["started"], check["finished"])
        if publish is not None:
            phases["publication"] += publish
            phases["diagnostic"] += collect
            phases["other"] += max(0.0, seconds - publish - collect)
        else:
            phases["other"] += seconds
        rows.append({"name": check["name"], "outcome": check["outcome"],
                     "passed": check["passed"], "seconds": seconds,
                     "publication": publish, "diagnostic": collect})
        cursor = check["finished"]
    phases["release"] += _span(cursor, finished)
    return {
        "suite": summary.get("suite"), "owner": summary.get("owner"),
        "passed": bool(summary.get("passed")), "directory": directory,
        "started": started["wall"], "finished": finished["wall"],
        "wall": _span(started, finished),
        "checks": rows,
        "phases": {name: round(value, 3) for name, value in phases.items()},
    }


def report(directories):
    suites = [suite_timing(directory) for directory in directories]
    start = min(suite["started"] for suite in suites)
    end = max(suite["finished"] for suite in suites)
    wall = round(end - start, 3)
    completed = sum(1 for suite in suites for check in suite["checks"]
                    if check["passed"])
    return {
        "suites": suites,
        "concurrent": len(suites),
        "wall": wall,
        "passed_checks": completed,
        "checks_per_hour": round(completed * 3600.0 / wall, 2) if wall > 0 else None,
    }


def render(result):
    lines = []
    for suite in result["suites"]:
        lines.append("suite %s owner=%s %s wall=%.1fs" % (
            suite["suite"], suite["owner"],
            "passed" if suite["passed"] else "failed", suite["wall"]))
        for name, seconds in suite["phases"].items():
            lines.append("  %-12s %8.1fs" % (name, seconds))
        for check in suite["checks"]:
            lines.append("  check %-10s %-14s %7.1fs  publish=%s diagnostic=%s" % (
                check["name"], check["outcome"], check["seconds"],
                "-" if check["publication"] is None else "%.1fs" % check["publication"],
                "-" if check["diagnostic"] is None else "%.1fs" % check["diagnostic"]))
    lines.append("guests=%d wall=%.1fs passed_checks=%d checks_per_hour=%s" % (
        result["concurrent"], result["wall"], result["passed_checks"],
        result["checks_per_hour"]))
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("report", help="print phase timings for finished suites")
    p.add_argument("suite", nargs="+", help="suite evidence directory (suite.json)")
    p.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        result = report(args.suite)
    except TimingError as exc:
        print("timing: %s" % exc, file=sys.stderr)
        return 2
    if args.json:
        json.dump(result, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
    else:
        print(render(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
