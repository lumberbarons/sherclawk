#!/usr/bin/env python3
"""Opt-in OS 9 acceptance on a mounted AFP export; not part of host discovery.

Build/publish first. The caller owns the guest and must reset it after unknown,
stopped or harness_error; this script never retries or reboots a guest.
"""
import argparse
import dataclasses
import json
import os
import secrets
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from contract import (ContractError, Request, filename, read_ready_clock,  # noqa: E402
                      read_report, write_request)
from mailbox import publish  # noqa: E402


def run(args):
    with open(os.path.join(args.mailbox, 'ready'), 'rb') as f:
        boot, clock = read_ready_clock(f.read(129), args.slot, args.previous_boot)
    diagnostic = {'write': 'SherclawkWriteCheck', 'inspect': 'SherclawkInspectCheck'}.get(
        args.case, 'HarnessAcceptance')
    run_id = secrets.token_hex(16)
    # Each case's directory is create-only; retain fixture contents and reports.
    workspace = os.path.join(args.runs, run_id[:20])
    os.mkdir(workspace)
    scenario = 'wait' if args.case in ('deadline', 'stop') else args.case
    publish(os.path.join(workspace, 'scenario'), scenario.encode('ascii'))
    seconds = 4 if args.case == 'deadline' else 120
    request = Request(args.slot, secrets.token_hex(16), boot, run_id, diagnostic,
                      args.applications + diagnostic, args.guest_runs + run_id[:20] + ':', clock + seconds)
    data = write_request(request)
    publish(os.path.join(workspace, 'request-evidence'), data)
    publish(os.path.join(args.mailbox, 'request'), data)
    if args.case == 'stop':
        # Prepublished Stop is valid even before launch, and never promises undo.
        publish(os.path.join(args.mailbox, filename(request, 'stop')), data)
    deadline = time.monotonic() + seconds + 15
    result_path = os.path.join(args.mailbox, filename(request, 'result'))
    while time.monotonic() < deadline:
        try:
            with open(result_path, 'rb') as f:
                report = read_report(f.read(32768), request)
            expected = {'fail': 'failed', 'early': 'unknown', 'partial': 'harness_error',
                        'identity': 'harness_error', 'target': 'harness_error', 'badconfig': 'unknown', 'deadline': 'unknown', 'stop': 'stopped'}.get(args.case, 'passed')
            with open(os.path.join(workspace, 'accepted.json'), 'x') as f:
                json.dump(dataclasses.asdict(report), f, indent=2)
            if report.outcome != expected:
                raise ContractError(f'expected {expected}, got {report.outcome}')
            if args.case == 'duplicate':
                before = os.stat(result_path).st_mtime_ns
                publish(os.path.join(args.mailbox, 'request'), data)
                time.sleep(3)
                if os.stat(result_path).st_mtime_ns != before or not os.path.exists(os.path.join(args.mailbox, 'request')):
                    raise ContractError('duplicate request was consumed or result replaced')
            print(f'{args.case}: {report.outcome}; checks={len(report.checks)}; run={run_id}; boot={boot}')
            return 0
        except FileNotFoundError:
            time.sleep(0.25)
    raise ContractError(f'no closed terminal result by deadline; unknown; retain {workspace}; do not replay')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('case', choices=('write', 'inspect', 'pass', 'fail', 'duplicate', 'early',
                                   'partial', 'identity', 'target', 'badconfig', 'deadline', 'stop'))
    p.add_argument('--slot', required=True, choices=('slot1', 'slot2'))
    p.add_argument('--mailbox', required=True, help='host mounted Retro68/Harness/<slot>')
    p.add_argument('--runs', required=True, help='host mounted private run parent')
    p.add_argument('--guest-runs', required=True, help='same run parent as HFS colon path')
    p.add_argument('--applications', default='Retro68:', help='published app folder as HFS path')
    p.add_argument('--previous-boot', help='require a fresh boot after recovery')
    args = p.parse_args()
    try:
        return run(args)
    except (OSError, ContractError) as e:
        print(str(e), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
