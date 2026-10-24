"""Phase timings and throughput are computed from retained evidence only."""
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import timing  # noqa: E402


def stamp(mono, wall=None):
    return {"monotonic": mono, "wall": 1000.0 + mono if wall is None else wall}


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f)


class TimingTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def evidence(self, name, steps):
        path = os.path.join(self.tmp.name, "evidence", name + ".json")
        write(path, {"steps": steps})
        return path

    def suite(self, name, offset=0.0, reset=False, evidence=True, outcome="passed"):
        """Boot 100 s, check one 40 s, optional 90 s reset/reboot, check two 30 s."""
        first = {"name": "one", "outcome": outcome, "passed": outcome == "passed",
                 "seconds": 40.0, "started": stamp(offset + 100),
                 "finished": stamp(offset + 140), "run": {"evidence": None}}
        second = {"name": "two", "outcome": "passed", "passed": True,
                  "seconds": 30.0,
                  "started": stamp(offset + (230 if reset else 140)),
                  "finished": stamp(offset + (260 if reset else 170)),
                  "run": {"evidence": None}}
        if evidence:
            first["run"]["evidence"] = self.evidence(name + "-one", [
                {"name": "application", "started": 0.0, "finished": 6.0},
                {"name": "fixture:scenario", "started": 6.0, "finished": 7.0},
                {"name": "request", "started": 7.0, "finished": 9.0},
                {"name": "collect", "started": 9.0, "finished": 38.0}])
            second["run"]["evidence"] = self.evidence(name + "-two", [
                {"name": "application", "started": 0.0, "finished": 5.0},
                {"name": "collect", "started": 5.0, "finished": 28.0},
                {"name": "never-finished", "started": 28.0}])
        reservations = [{"at": stamp(offset + 100), "reason": "suite start"}]
        if reset:
            reservations.append({"at": stamp(offset + 230), "reason": "reset"})
        end = offset + (270 if reset else 180)
        directory = os.path.join(self.tmp.name, name)
        write(os.path.join(directory, timing.SUMMARY_NAME), {
            "suite": name, "owner": "agent-" + name, "passed": True,
            "started": stamp(offset), "finished": stamp(end),
            "checks": [first, second], "reservations": reservations})
        return directory

    def test_phases_split_boot_publication_diagnostic_and_release(self):
        result = timing.suite_timing(self.suite("a"))
        self.assertEqual(result["wall"], 180.0)
        self.assertEqual(result["phases"], {
            "boot": 100.0, "publication": 14.0, "diagnostic": 52.0, "reset": 0.0,
            "release": 10.0, "other": 4.0})
        self.assertEqual(sum(result["phases"].values()), result["wall"])
        self.assertEqual([c["publication"] for c in result["checks"]], [9.0, 5.0])

    def test_a_reset_between_checks_is_its_own_phase(self):
        result = timing.suite_timing(self.suite("a", reset=True))
        self.assertEqual(result["phases"]["boot"], 100.0)
        self.assertEqual(result["phases"]["reset"], 90.0)
        self.assertEqual(sum(result["phases"].values()), result["wall"])

    def test_missing_evidence_counts_the_whole_check_as_other(self):
        result = timing.suite_timing(self.suite("a", evidence=False))
        self.assertEqual(result["phases"]["other"], 70.0)
        self.assertIsNone(result["checks"][0]["publication"])

    def test_two_concurrent_suites_report_combined_wall_and_rate(self):
        one = timing.report([self.suite("a")])
        both = timing.report([self.suite("b"), self.suite("c", offset=10.0)])
        self.assertEqual(one["checks_per_hour"], 40.0)
        self.assertEqual(both["wall"], 190.0)
        self.assertEqual(both["passed_checks"], 4)
        self.assertEqual(both["checks_per_hour"], round(4 * 3600 / 190.0, 2))
        self.assertGreater(both["checks_per_hour"], one["checks_per_hour"])

    def test_failed_checks_do_not_count_toward_throughput(self):
        result = timing.report([self.suite("a", outcome="failed")])
        self.assertEqual(result["passed_checks"], 1)

    def test_an_unfinished_suite_is_refused(self):
        directory = os.path.join(self.tmp.name, "open")
        write(os.path.join(directory, timing.SUMMARY_NAME),
              {"started": stamp(0), "finished": None, "checks": []})
        with self.assertRaisesRegex(timing.TimingError, "not finished"):
            timing.suite_timing(directory)

    def test_a_suite_that_failed_before_its_first_check_still_reports_its_boot(self):
        directory = os.path.join(self.tmp.name, "early")
        write(os.path.join(directory, timing.SUMMARY_NAME), {
            "suite": "early", "owner": "agent", "passed": False,
            "started": stamp(0), "finished": stamp(60), "checks": [],
            "reservations": [{"at": stamp(50), "reason": "suite start"}]})
        phases = timing.suite_timing(directory)["phases"]
        self.assertEqual((phases["boot"], phases["release"]), (50.0, 10.0))

    def test_unreadable_run_evidence_is_called_out_not_hidden(self):
        directory = self.suite("a", evidence=False)
        with open(os.path.join(directory, timing.SUMMARY_NAME), encoding="utf-8") as f:
            data = json.load(f)
        data["checks"][0]["run"]["evidence"] = os.path.join(self.tmp.name, "gone.json")
        write(os.path.join(directory, timing.SUMMARY_NAME), data)
        result = timing.suite_timing(directory)
        self.assertEqual(result["unreadable_evidence"], ["one"])
        self.assertIn("evidence unreadable for one", timing.render(timing.report([directory])))

    def test_suites_that_ran_one_after_another_are_flagged(self):
        both = timing.report([self.suite("b"), self.suite("c", offset=10.0)])
        self.assertTrue(both["overlapping"])
        self.assertNotIn("did not overlap", timing.render(both))
        apart = timing.report([self.suite("d"), self.suite("e", offset=500.0)])
        self.assertFalse(apart["overlapping"])
        self.assertIn("did not overlap", timing.render(apart))

    def test_the_combined_wall_uses_the_same_clock_as_each_suite(self):
        directory = self.suite("a")
        with open(os.path.join(directory, timing.SUMMARY_NAME), encoding="utf-8") as f:
            data = json.load(f)
        data["started"]["wall"] += 99999.0   # a stepped wall clock must not matter
        data["finished"]["wall"] += 99999.0
        write(os.path.join(directory, timing.SUMMARY_NAME), data)
        self.assertEqual(timing.report([directory])["wall"], 180.0)

    def test_a_missing_directory_is_an_error(self):
        with self.assertRaisesRegex(timing.TimingError, "cannot read"):
            timing.suite_timing(os.path.join(self.tmp.name, "nowhere"))

    def test_cli_prints_text_and_json_and_exits_two_on_error(self):
        directory = self.suite("a")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(timing.main(["report", directory]), 0)
        self.assertIn("checks_per_hour=40.0", out.getvalue())
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(timing.main(["report", "--json", directory]), 0)
        self.assertEqual(json.loads(out.getvalue())["concurrent"], 1)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(timing.main(["report", self.tmp.name + "/nowhere"]), 2)
        self.assertIn("timing:", err.getvalue())


if __name__ == "__main__":
    unittest.main()
