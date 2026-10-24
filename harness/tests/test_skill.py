"""The packaged agent skill must not drift from the commands and states it names."""
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import manage  # noqa: E402
import reservation  # noqa: E402
import suite  # noqa: E402

HARNESS = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SKILL = os.path.join(HARNESS, "skills", "os9-guests", "SKILL.md")


def subcommands(parser):
    for action in parser._actions:
        if getattr(action, "choices", None) and hasattr(action, "_name_parser_map"):
            return set(action.choices)
    return set()


class SkillTest(unittest.TestCase):
    def setUp(self):
        with open(SKILL, encoding="utf-8") as f:
            self.text = f.read()

    def test_frontmatter_names_the_skill_and_when_to_use_it(self):
        match = re.match(r"---\nname: (\S+)\ndescription: (.+)\n---\n", self.text)
        self.assertIsNotNone(match)
        self.assertEqual(match.group(1), os.path.basename(os.path.dirname(SKILL)))
        self.assertIn("how many guests", match.group(2))

    def test_every_manage_and_suite_subcommand_it_runs_exists(self):
        manage_commands = subcommands(manage.build_parser())
        suite_commands = subcommands(suite.build_parser())
        for command in re.findall(r"harness/manage\.py (?:--json )?([a-z]+)", self.text):
            self.assertIn(command, manage_commands)
        for command in re.findall(r"harness/suite\.py ([a-z]+)", self.text):
            self.assertIn(command, suite_commands)
        for command in ("status", "reserve", "release", "recover"):
            self.assertIn("harness/manage.py " + command, self.text.replace("--json ", ""))

    def test_every_repository_path_it_names_exists(self):
        root = os.path.dirname(HARNESS)
        for path in re.findall(r"`?((?:harness|tools)/[\w./-]+\.(?:py|md|json))", self.text):
            self.assertTrue(os.path.exists(os.path.join(root, path)), path)

    def test_the_slot_states_it_lists_are_real(self):
        line = next(line for line in self.text.splitlines() if "`launching`" in line)
        named = set(re.findall(r"`([a-z]+)`", line)) - {"free"}
        real = set(reservation.SLOT_STATES) | {"expired"}
        self.assertTrue(named)
        self.assertLessEqual(named, real)

    def test_it_carries_the_driving_and_recovery_rules(self):
        lowered = self.text.lower()
        for phrase in ("one qmp client", "lags one action", "button held",
                       "special > shut down", "quarantined", "recover --slot"):
            self.assertIn(phrase, lowered)


if __name__ == "__main__":
    unittest.main()
