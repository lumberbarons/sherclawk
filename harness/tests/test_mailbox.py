"""Closed publication and CLI correlation tests on a temporary mounted share."""
import contextlib
import io
import os
import sys
import tempfile
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
