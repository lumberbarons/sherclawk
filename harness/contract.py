"""Public HARNESS/1 closed-file contract (also implemented in native/contract.c).

Wire records are ASCII terminated by CR, including END. There is no escaping.
Deadlines use the guest clock (Mac OS seconds since 1904), read from readiness. A missing/invalid report is never a pass.
"""
import dataclasses
import re

REQUEST_CAP = 1024
REPORT_CAP = 32768
CHECK_MAX = 256
DIAGNOSTICS = ("SherclawkWriteCheck", "SherclawkInspectCheck", "HarnessAcceptance")
OUTCOMES = ("passed", "failed", "harness_error", "stopped", "unknown")


class ContractError(ValueError):
    pass


@dataclasses.dataclass(frozen=True)
class Request:
    slot: str
    generation: str
    boot: str
    run: str
    diagnostic: str
    application: str
    workspace: str
    deadline: int


@dataclasses.dataclass(frozen=True)
class Report:
    request: Request
    outcome: str
    checks: list


def _records(data, cap):
    if len(data) >= cap or not data.endswith(b'\r'):
        raise ContractError("oversized or unclosed file")
    try:
        text = data.decode('ascii')
    except UnicodeDecodeError as e:
        raise ContractError("wire data must be ASCII") from e
    if any(ord(c) < 32 and c != '\r' or ord(c) > 126 for c in text):
        raise ContractError("invalid record character")
    return text[:-1].split('\r')


def _path(s, directory):
    return (2 < len(s) <= 220 and ':' in s and not s.startswith(':')
            and '::' not in s and s.endswith(':') == directory
            and all(len(part) <= 31 for part in s.split(':'))
            and all(32 <= ord(c) <= 126 for c in s))


def _request(lines):
    if len(lines) != 9 or lines[0] != 'HARNESS/1':
        raise ContractError("invalid request header/version")
    if not re.fullmatch(r'[0-9]{1,10}', lines[8]):
        raise ContractError("invalid deadline")
    r = Request(*lines[1:8], int(lines[8]))
    if (r.slot not in ('slot1', 'slot2')
            or any(not re.fullmatch('[0-9a-f]{32}', s) for s in (r.generation, r.boot, r.run))
            or r.diagnostic not in DIAGNOSTICS
            or not _path(r.application, False) or r.application.split(':')[-1] != r.diagnostic
            or not _path(r.workspace, True) or not 0 < r.deadline <= 4294967295):
        raise ContractError("invalid request identity, diagnostic, paths or deadline")
    return r


def _request_lines(r):
    lines = ['HARNESS/1'] + [str(getattr(r, f.name)) for f in dataclasses.fields(r)]
    _request(lines)
    return lines


def _encode(lines, cap):
    try:
        data = ('\r'.join(lines) + '\r').encode('ascii')
    except UnicodeEncodeError as e:
        raise ContractError("wire data must be ASCII") from e
    if _records(data, cap) != lines:
        raise ContractError("embedded record separator")
    return data


def read_request(data):
    lines = _records(data, REQUEST_CAP)
    if len(lines) != 10 or lines[-1] != 'END':
        raise ContractError("invalid request framing")
    return _request(lines[:-1])


def write_request(r):
    return _encode(_request_lines(r) + ['END'], REQUEST_CAP)


def _validate_report(r):
    failures = sum(not ok for ok, _ in r.checks)
    if (r.outcome not in OUTCOMES or len(r.checks) > CHECK_MAX
            or r.outcome == 'passed' and (not r.checks or failures)
            or r.outcome == 'failed' and not failures):
        raise ContractError("invalid outcome/check counts")
    for ok, name in r.checks:
        if type(ok) is not bool or not 0 < len(name) <= 120 or any(not 32 <= ord(c) <= 126 for c in name):
            raise ContractError("invalid named assertion")
    return failures


def write_report(r):
    failures = _validate_report(r)
    return _encode(_request_lines(r.request) + [r.outcome]
                   + [f'{int(ok)} {name}' for ok, name in r.checks]
                   + [f'TOTAL {len(r.checks)} {failures}', 'END'], REPORT_CAP)


def read_report(data, expected):
    lines = _records(data, REPORT_CAP)
    if len(lines) < 12 or lines[-1] != 'END' or _request(lines[:9]) != expected:
        raise ContractError("invalid report framing or mismatched identity")
    checks = []
    for line in lines[10:-2]:
        if not re.fullmatch(r'[01] .{1,120}', line):
            raise ContractError("invalid assertion record")
        checks.append((line[0] == '1', line[2:]))
    r = Report(expected, lines[9], checks)
    failures = _validate_report(r)
    if lines[-2] != f'TOTAL {len(checks)} {failures}':
        raise ContractError("conflicting assertion totals")
    return r


def read_ready_clock(data, slot, previous_boot=None):
    lines = _records(data, 128)
    if (len(lines) != 5 or lines[0] != 'READY/1' or lines[1] != slot or lines[4] != 'END'
            or not re.fullmatch('[0-9a-f]{32}', lines[2]) or lines[2] == previous_boot
            or not re.fullmatch('[0-9]{1,10}', lines[3]) or not 0 < int(lines[3]) <= 4294967295):
        raise ContractError("invalid or stale readiness")
    return lines[2], int(lines[3])


def read_ready(data, slot, previous_boot=None):
    return read_ready_clock(data, slot, previous_boot)[0]


def filename(r, kind):
    if kind not in ('claim', 'report', 'result', 'stop'):
        raise ContractError("unknown mailbox record")
    return f'{kind}-{r.run[:20]}'
