"""Exercise job publication, execution, exclusivity and crash non-replay."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("publisher", ROOT / "tools/publish-worker-job.py")
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


class WorkerChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.queue = self.root / "queue"
        self.queue.mkdir()

    def publish(self, job_id, text):
        script = self.root / "input.sh"
        script.write_text(text)
        return publisher.publish(self.queue, job_id, script)

    def command(self, once=True):
        return ["perl", str(ROOT / "worker/worker.pl"), str(self.queue)] + (["--once"] if once else [])

    def run_worker(self):
        return subprocess.run(self.command(), capture_output=True, timeout=10)

    def result(self, job):
        return dict(line.split("=", 1) for line in (job / "result").read_text().splitlines())

    def test_success_failure_and_no_replay(self):
        good = self.publish("good", "echo stage=prepare\necho diagnostic >&2\necho x >> count\n")
        bad = self.publish("bad", "echo compiler-error >&2\nexit 7\n")
        self.assertEqual(self.run_worker().returncode, 0)
        self.assertEqual(self.result(good)["outcome"], "succeeded")
        self.assertEqual(self.result(bad)["exit"], "7")
        self.assertEqual((good / "stdout").read_text(), "stage=prepare\n")
        self.assertEqual((bad / "stderr").read_text(), "compiler-error\n")
        self.assertFalse((good / "ready").exists())
        self.assertTrue((good / "claimed").exists())
        self.assertEqual(self.run_worker().returncode, 0)
        self.assertEqual((good / "count").read_text(), "x\n")
        with self.assertRaises(FileExistsError):
            self.publish("good", "exit 0\n")

    def test_incomplete_and_rejected(self):
        staged = self.queue / "staged"
        staged.mkdir()
        (staged / "script").write_text("touch unexpected\n")
        malformed = self.publish("malformed", "touch unexpected\n")
        (malformed / "ready").write_text("protocol=2\n")
        linked = self.publish("linked", "touch unexpected\n")
        (linked / "script").unlink()
        (linked / "script").symlink_to(staged / "script")
        cr = self.publish("cr", "touch unexpected\n")
        (cr / "script").write_bytes(b"touch unexpected\r")
        oversized = self.publish("oversized", "touch unexpected\n")
        (oversized / "script").write_bytes(b"#" * 65537)
        nul = self.publish("nul", "touch unexpected\n")
        (nul / "script").write_bytes(b"touch unexpected\0\n")
        self.assertEqual(self.run_worker().returncode, 0)
        self.assertFalse((staged / "claimed").exists())
        for job in (malformed, linked, cr, oversized, nul):
            self.assertEqual(self.result(job)["outcome"], "rejected")
            self.assertFalse((job / "unexpected").exists())

    def test_stop_and_conflicting_outputs(self):
        job = self.publish("good", "touch unexpected\n")
        (self.queue / "STOP").write_text("")
        self.assertEqual(self.run_worker().returncode, 0)
        self.assertTrue((job / "ready").exists())
        (self.queue / "STOP").unlink()
        (job / "stdout").write_text("preserved")
        self.assertNotEqual(self.run_worker().returncode, 0)
        self.assertFalse((job / "result").exists())
        self.assertEqual((job / "stdout").read_text(), "preserved")
        self.assertTrue((self.queue / "worker-lock").exists())

    def test_signal(self):
        job = self.publish("signal", "kill -TERM $$\n")
        self.assertEqual(self.run_worker().returncode, 0)
        self.assertEqual(self.result(job)["outcome"], "signaled")
        self.assertEqual(self.result(job)["signal"], "15")

    def test_live_publication_and_stop_finishes_current_job(self):
        worker = subprocess.Popen(self.command(False), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 5
            while not (self.queue / "worker-lock").exists() and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertTrue((self.queue / "worker-lock").exists())
            active = self.publish("active", "echo running > active\nsleep 1\n")
            deadline = time.monotonic() + 5
            while not (active / "active").exists() and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertTrue((active / "active").exists())
            pending = self.publish("zzpending", "touch unexpected\n")
            (self.queue / "STOP").write_text("")
            self.assertEqual(worker.wait(timeout=5), 0)
            self.assertEqual(self.result(active)["outcome"], "succeeded")
            self.assertTrue((pending / "ready").exists())
            self.assertFalse((pending / "claimed").exists())
            self.assertFalse((self.queue / "worker-lock").exists())
        finally:
            if worker.poll() is None:
                worker.kill()
                worker.wait(timeout=5)

    def test_lock_and_crash_preserve_unknown_outcome(self):
        job = self.publish("crash", "echo x >> count\nsleep 1\n")
        worker = subprocess.Popen(self.command(False), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 5
            while not (job / "count").exists() and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertTrue((job / "count").exists())
            self.assertNotEqual(self.run_worker().returncode, 0)
            worker.kill()
            worker.wait(timeout=5)
            time.sleep(1.1)  # Allow its shell child to finish; no result can be committed.
            self.assertFalse((job / "result").exists())
            self.assertTrue((job / "claimed").exists())
            self.assertNotEqual(self.run_worker().returncode, 0)
            (self.queue / "worker-lock").rmdir()  # Explicit operator recovery.
            self.assertEqual(self.run_worker().returncode, 0)
            self.assertEqual((job / "count").read_text(), "x\n")
            self.assertFalse((job / "result").exists())
        finally:
            if worker.poll() is None:
                worker.kill()
                worker.wait(timeout=5)

    def test_publication_validation(self):
        for job_id in ("../escape", "UPPER", "worker-lock", "a" * 25):
            with self.assertRaises(ValueError):
                self.publish(job_id, "exit 0\n")
        with self.assertRaises(ValueError):
            self.publish("cr", "echo x\rexit 0\n")
        self.assertFalse((self.queue / "cr").exists())


if __name__ == "__main__":
    unittest.main()
