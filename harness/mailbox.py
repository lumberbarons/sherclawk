#!/usr/bin/env python3
"""Prepare or validate one HARNESS/1 mailbox exchange on a mounted share.

This is the contract pilot CLI, not the reservation manager (#71). Build and
publish diagnostics before touching a guest; never retry an uncertain launch.
"""
import argparse
import dataclasses
import json
import os
import secrets
import sys
import time

from contract import (ContractError, DIAGNOSTICS, Request, filename,
                      read_ready, read_ready_clock, read_report, read_request, write_request)


def publish(path, data):
    staging = path + '.tmp'
    # O_EXCL and link make both staging and final publication create-only.
    with open(staging, 'xb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.link(staging, path)
    os.unlink(staging)


def wait_ready(mailbox, slot, previous_boot=None, timeout=10):
    """Admit only a live heartbeat; missing/partial replacement files can retry.

    No request has been published yet, so this wait cannot replay a launch.
    """
    deadline = time.monotonic() + timeout
    first = None
    while time.monotonic() < deadline:
        try:
            with open(os.path.join(mailbox, 'ready'), 'rb') as f:
                current = read_ready_clock(f.read(129), slot, previous_boot)
            if first and current[0] == first[0] and current[1] > first[1]:
                return current
            first = current
        except (OSError, ContractError):
            pass
        time.sleep(min(0.1, max(0, deadline-time.monotonic())))
    raise ContractError('no advancing fresh controller heartbeat')


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = p.add_subparsers(dest='command', required=True)
    ready = sub.add_parser('ready')
    ready.add_argument('mailbox')
    ready.add_argument('slot', choices=('slot1', 'slot2'))
    ready.add_argument('--previous-boot')
    request = sub.add_parser('request')
    request.add_argument('mailbox')
    request.add_argument('slot', choices=('slot1', 'slot2'))
    request.add_argument('boot')
    request.add_argument('generation', help='32 lowercase hex digits')
    request.add_argument('diagnostic', choices=DIAGNOSTICS)
    request.add_argument('application', help='exact guest HFS path')
    request.add_argument('workspace', help='private guest HFS path, ending in colon')
    request.add_argument('--seconds', type=int, default=120)
    request.add_argument('--save', required=True, help='local request evidence (create-only)')
    for command in ('result', 'stop'):
        command_parser = sub.add_parser(command)
        command_parser.add_argument('mailbox')
        command_parser.add_argument('request', help='saved request evidence')
    args = p.parse_args(argv)
    try:
        if args.command == 'ready':
            with open(os.path.join(args.mailbox, 'ready'), 'rb') as f:
                print(read_ready(f.read(129), args.slot, args.previous_boot))
        elif args.command == 'request':
            if not 1 <= args.seconds <= 600:
                raise ContractError('deadline must be within 1..600 seconds')
            with open(os.path.join(args.mailbox, 'ready'), 'rb') as f:
                boot, clock = read_ready_clock(f.read(129), args.slot)
            if boot != args.boot:
                raise ContractError('controller boot changed')
            r = Request(args.slot, args.generation, args.boot, secrets.token_hex(16),
                        args.diagnostic, args.application, args.workspace,
                        clock + args.seconds)
            data = write_request(r)
            publish(args.save, data)
            publish(os.path.join(args.mailbox, 'request'), data)
            print(r.run)
        else:
            with open(args.request, 'rb') as f:
                r = read_request(f.read(1024))
            if args.command == 'stop':
                publish(os.path.join(args.mailbox, filename(r, 'stop')), write_request(r))
            else:
                with open(os.path.join(args.mailbox, filename(r, 'result')), 'rb') as f:
                    report = read_report(f.read(32768), r)
                print(json.dumps(dataclasses.asdict(report)))
                return 0 if report.outcome == 'passed' else 1
    except (OSError, ContractError) as e:
        print(str(e), file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
