"""Two agents on two slots: isolation, stale owners, stalls and fresh workspaces.

The pieces are covered one by one in test_reservation, test_publish and
test_suite. These scenarios run two whole suites against one manager, with the
second suite executing inside the first one's wait for its result, so what is
under test is the interaction: one agent's failure, stall or stale token must
leave the other agent's guest, lease and evidence alone. No QEMU, share host or
guest is used.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import publish  # noqa: E402
import reservation  # noqa: E402
import suite  # noqa: E402
from test_suite import SuiteBase, check, write_registry  # noqa: E402


class TwoAgentTest(SuiteBase):
    def run_beside(self, first_entries, first_specs, second_entries, second_specs,
                   before_second=None, after_second=None):
        """Run agent A's suite; agent B's whole suite runs during A's first collect."""
        outcome = {}
        publisher = self.publisher
        # begin() pops specs in call order: A's first check, then all of B's.
        publisher.specs = list(first_specs) + list(second_specs)

        def beside(run):
            publisher.on_collect = None
            if before_second is not None:
                before_second()
            outcome["owners_before_b"] = [
                slot["owner"] for slot in self.manager.status()["slots"]
                if slot["owner"]]
            second = self.runner()
            outcome["b"] = second.run(
                self.registry_for(second_entries, "second"), owner="bob",
                artifacts=self.artifacts(*{e["target"] for e in second_entries}))
            if after_second is not None:
                after_second()

        publisher.on_collect = beside
        runner = self.runner()
        outcome["a"] = runner.run(
            self.registry_for(first_entries, "first"), owner="alice",
            artifacts=self.artifacts(*{e["target"] for e in first_entries}))
        return outcome

    def registry_for(self, entries, name):
        directory = os.path.join(self.tmp.name, name)
        os.makedirs(directory, exist_ok=True)
        return suite.load_registry(write_registry(directory, entries))

    def test_a_deliberate_failure_in_one_suite_leaves_the_other_untouched(self):
        outcome = self.run_beside(
            [check("one"), check("two", target="SherclawkSearchCheck")],
            [{"outcome": "failed", "checks": ((False, "deliberate assertion"),)}],
            [check("write"), check("inspect", target="SherclawkInspectCheck")], [])
        a, b = outcome["a"], outcome["b"]
        self.assertFalse(a["passed"])
        self.assertTrue(a["stopped_early"])
        self.assertEqual(a["checks"][0]["failed_assertions"], ["deliberate assertion"])
        self.assertTrue(b["passed"], b)
        self.assertEqual(outcome["owners_before_b"], ["alice"])
        runs = [a["checks"][0]["run"]] + [c["run"] for c in b["checks"]]
        self.assertNotEqual(runs[0]["slot"], runs[1]["slot"])
        self.assertEqual(runs[1]["slot"], runs[2]["slot"])
        workspaces = [run["workspace"] for run in runs]
        self.assertEqual(len(workspaces), len(set(workspaces)))
        for run in runs:
            self.assertIn("%s-g%d" % (run["slot"], run["generation"]), run["workspace"])
        self.assertNotEqual(a["evidence_dir"], b["evidence_dir"])
        self.assertEqual([slot["state"] for slot in self.manager.status()["slots"]],
                         ["free", "free"])

    def test_a_stalled_guest_is_unknown_for_one_agent_and_invisible_to_the_other(self):
        outcome = self.run_beside(
            [check("one")], [{"raise": publish.RunTimeout("no closed result")}],
            [check("write")], [])
        self.assertEqual(outcome["a"]["checks"][0]["outcome"], "unknown")
        self.assertFalse(outcome["a"]["passed"])
        self.assertTrue(outcome["b"]["passed"])
        # Neither check was replayed on its slot.
        self.assertEqual(len(self.publisher.begins), 2)
        self.assertEqual([slot["state"] for slot in self.manager.status()["slots"]],
                         ["free", "free"])

    def test_a_released_owners_token_cannot_touch_a_running_suite(self):
        stale = {}

        def release_then_try_everything():
            # A third agent held a slot, released it, and now replays its token
            # while bob's suite owns that slot's new generation.
            lease = self.manager.reserve("carol")
            stale["lease"] = lease
            self.manager.release(lease.token, "carol")
            stale["current"] = self.manager.reserve("dave")

        def stale_calls():
            lease = stale["lease"]
            for name, call in (
                    ("renew", lambda: self.manager.renew(lease.token, "carol")),
                    ("screenshot", lambda: self.manager.screenshot(lease.token, "carol")),
                    ("input", lambda: self.manager.input(lease.token, "carol", text="x"))):
                with self.subTest(call=name):
                    with self.assertRaises(reservation.LeaseError):
                        call()
            self.assertEqual(
                self.manager.release(lease.token, "carol")["already_released"], True)
            self.manager.release(stale["current"].token, "dave")

        # Alice keeps one slot; carol cycles the other and dave takes it, so bob
        # is refused for capacity rather than handed alice's or dave's guest.
        first = self.runner()
        outcome = {}

        def beside(run):
            self.publisher.on_collect = None
            release_then_try_everything()
            second = self.runner()
            with self.assertRaises(reservation.NoCapacity):
                second.run(self.registry_for([check("write")], "second"), owner="bob",
                           artifacts=self.artifacts("SherclawkWriteCheck"))
            stale_calls()
            outcome["alice_still_holds"] = [
                slot["owner"] for slot in self.manager.status()["slots"]]

        self.publisher.on_collect = beside
        outcome["a"] = first.run(self.registry_for([check("one")], "first"),
                                 owner="alice",
                                 artifacts=self.artifacts("SherclawkWriteCheck"))
        self.assertTrue(outcome["a"]["passed"], outcome["a"])
        self.assertIn("alice", outcome["alice_still_holds"])

    def test_repeated_runs_never_reuse_a_workspace_or_a_boot(self):
        summaries = [self.run_suite([check("write")]) for _ in range(3)]
        runs = [summary["checks"][0]["run"] for summary in summaries]
        self.assertTrue(all(summary["passed"] for summary in summaries))
        self.assertEqual(len({run["workspace"] for run in runs}), 3)
        self.assertEqual(len({run["id"] for run in runs}), 3)
        reserved = [summary["reservations"][0] for summary in summaries]
        self.assertEqual(len({(r["slot"], r["generation"]) for r in reserved}), 3)
        self.assertEqual(len({r["boot"] for r in reserved}), 3)

    def test_an_expired_agents_slot_is_reassigned_and_its_release_is_a_no_op(self):
        outcome = self.run_beside(
            [check("one")], [], [check("write")], [],
            before_second=lambda: self.clock.advance(reservation.LEASE_SECONDS + 1))
        a, b = outcome["a"], outcome["b"]
        a_run, b_run = a["checks"][0]["run"], b["checks"][0]["run"]
        # Bob's reserve recovered alice's expired row and took over her slot.
        self.assertEqual(b_run["slot"], a_run["slot"])
        self.assertGreater(b_run["generation"], a_run["generation"])
        self.assertTrue(b["passed"], b)
        self.assertTrue(a["releases"][0]["result"]["already_released"])
        self.assertEqual([slot["state"] for slot in self.manager.status()["slots"]],
                         ["free", "free"])


if __name__ == "__main__":
    unittest.main()
