"""Request/report acceptance: no VM, dependencies or API key."""
import dataclasses
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import contract  # noqa: E402


def request():
    return contract.Request("slot1", "1" * 32, "2" * 32, "3" * 32,
                            "SherclawkWriteCheck", "Retro68:Harness:r:SherclawkWriteCheck",
                            "Retro68:Harness:r:workspace:", 4000000000)


class ContractTest(unittest.TestCase):
    def test_request_roundtrip_and_unknown_version(self):
        r = request()
        self.assertEqual(contract.read_request(contract.write_request(r)), r)
        with self.assertRaises(contract.ContractError):
            contract.read_request(contract.write_request(r).replace(b'HARNESS/1', b'HARNESS/2'))

    def test_partial_trailing_oversized_and_nonascii_requests_fail(self):
        data = contract.write_request(request())
        for bad in (data[:-1], data + b'junk\r', data + b'x' * 1024, data.replace(b'slot1', b'slot\xff')):
            with self.subTest(bad=bad[:30]), self.assertRaises(contract.ContractError):
                contract.read_request(bad)

    def test_identity_path_and_allowlist_validation(self):
        r = request()
        for field, value in (("slot", "slot3"), ("run", "bad"), ("deadline", 0),
                             ("diagnostic", "OtherApp"), ("workspace", "Retro68::escape:"),
                             ("application", "Retro68:OtherApp")):
            with self.subTest(field=field), self.assertRaises(contract.ContractError):
                contract.write_request(dataclasses.replace(r, **{field: value}))

    def test_every_suite_diagnostic_is_registered_and_unknown_names_fail(self):
        suite = ("SherclawkWriteCheck", "SherclawkSearchCheck", "SherclawkEditCheck",
                 "SherclawkInspectCheck", "SherclawkProjectCheck", "SherclawkScrollCheck",
                 "SherclawkHandoffCheck", "SherclawkSelfBuildCheck",
                 "SherclawkSelfBuildStopCheck")
        for name in suite + ("HarnessAcceptance",):
            with self.subTest(name=name):
                r = dataclasses.replace(
                    request(), diagnostic=name,
                    application="Retro68:Harness:r:" + name)
                self.assertEqual(contract.read_request(contract.write_request(r)), r)
        for name in ("SherclawkBuildCheck", "SherclawkWhateverCheck"):
            with self.subTest(name=name), self.assertRaises(contract.ContractError):
                contract.write_request(dataclasses.replace(
                    request(), diagnostic=name,
                    application="Retro68:Harness:r:" + name))

    def test_named_pass_and_deliberate_failure(self):
        for checks, outcome in (([(True, "write file")], "passed"),
                                ([(True, "write file"), (False, "read back")], "failed")):
            report = contract.Report(request(), outcome, checks)
            self.assertEqual(contract.read_report(contract.write_report(report), request()), report)

    def test_wrong_identity_never_passes(self):
        data = contract.write_report(contract.Report(request(), "passed", [(True, "ok")]))
        r = request()
        for field in ("slot", "generation", "boot", "run", "application", "workspace", "deadline", "diagnostic"):
            value = getattr(r, field)
            changed = value + 1 if isinstance(value, int) else value + "x"
            with self.subTest(field=field), self.assertRaises(contract.ContractError):
                contract.read_report(data, dataclasses.replace(r, **{field: changed}))

    def test_partial_conflicting_counts_empty_and_false_pass_fail(self):
        data = contract.write_report(contract.Report(request(), "passed", [(True, "ok")]))
        for bad in (data[:-1], data + b'junk\r', data.replace(b'TOTAL 1 0', b'TOTAL 2 0'),
                    data.replace(b'1 ok', b'0 ok'), data.replace(b'1 ok\rTOTAL 1 0', b'TOTAL 0 0')):
            with self.subTest(bad=bad[-40:]), self.assertRaises(contract.ContractError):
                contract.read_report(bad, request())

    def test_nonpassing_terminal_outcomes_are_preserved(self):
        for outcome in ("unknown", "stopped", "harness_error"):
            r = contract.Report(request(), outcome, [])
            self.assertEqual(contract.read_report(contract.write_report(r), request()).outcome, outcome)

    def test_filenames_fit_hfs_including_staging_suffix(self):
        for kind in ("claim", "report", "result", "stop"):
            self.assertLessEqual(len(contract.filename(request(), kind) + '.tmp'), 31)


class ReadinessTest(unittest.TestCase):
    def test_fresh_boot_and_guest_clock(self):
        boot = 'a' * 32
        data = f'READY/1\rslot1\r{boot}\r4000000000\rEND\r'.encode('ascii')
        self.assertEqual(contract.read_ready_clock(data, 'slot1'), (boot, 4000000000))
        for slot, previous in (('slot2', None), ('slot1', boot)):
            with self.assertRaises(contract.ContractError):
                contract.read_ready(data, slot, previous)
        with self.assertRaises(contract.ContractError):
            contract.read_ready(data[:-1], 'slot1')
