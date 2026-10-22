"""Closed publication and CLI correlation tests on a temporary mounted share."""
import contextlib
import io
import os
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import contract  # noqa: E402
import mailbox as harness_mailbox  # noqa: E402


class PublicationTest(unittest.TestCase):
    def test_publication_never_overwrites_a_closed_request(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'request')
            harness_mailbox.publish(p, b'first')
            with self.assertRaises(FileExistsError):
                harness_mailbox.publish(p, b'second')
            with open(p, 'rb') as f:
                self.assertEqual(f.read(), b'first')
            self.assertTrue(os.path.exists(p + '.tmp'))  # retain failed publication evidence

    def test_staging_collision_cannot_replace_evidence(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'request')
            with open(p + '.tmp', 'wb') as f:
                f.write(b'partial prior publication')
            with self.assertRaises(FileExistsError):
                harness_mailbox.publish(p, b'new')
            self.assertFalse(os.path.exists(p))

    def test_cli_deadline_uses_guest_clock_and_saves_exact_request(self):
        with tempfile.TemporaryDirectory() as d, contextlib.redirect_stdout(io.StringIO()):
            boot = 'b' * 32
            with open(os.path.join(d, 'ready'), 'wb') as f:
                f.write(f'READY/1\rslot1\r{boot}\r3000000000\rEND\r'.encode('ascii'))
            saved = os.path.join(d, 'saved')
            args = ['request', d, 'slot1', boot, 'a' * 32, 'SherclawkWriteCheck',
                    'Retro68:r:SherclawkWriteCheck', 'Retro68:r:workspace:', '--save', saved]
            self.assertEqual(harness_mailbox.main(args), 0)
            with open(saved, 'rb') as f:
                data = f.read()
            r = contract.read_request(data)
            self.assertEqual(r.deadline, 3000000120)
            with open(os.path.join(d, 'request'), 'rb') as f:
                self.assertEqual(data, f.read())
            self.assertEqual(harness_mailbox.main(['stop', d, saved]), 0)
            with open(os.path.join(d, contract.filename(r, 'stop')), 'rb') as f:
                self.assertEqual(data, f.read())


class ReadinessAdmissionTest(unittest.TestCase):
    def test_static_readiness_cannot_admit_a_request(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, 'ready'), 'wb') as f:
                f.write(('READY/1\rslot1\r' + 'b'*32 + '\r3000000000\rEND\r').encode())
            with self.assertRaises(contract.ContractError):
                harness_mailbox.wait_ready(d, 'slot1', timeout=0.02)

    def test_advancing_same_boot_heartbeat_is_admitted(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'ready')
            def write(clock):
                with open(path, 'wb') as f:
                    f.write(f'READY/1\rslot1\r{"b"*32}\r{clock}\rEND\r'.encode())
            write(3000000000)
            def advance():
                time.sleep(0.05)
                write(3000000001)
            worker = threading.Thread(target=advance)
            worker.start()
            try:
                self.assertEqual(harness_mailbox.wait_ready(d, 'slot1', timeout=2), ('b'*32, 3000000001))
            finally:
                worker.join()
