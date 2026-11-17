import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release_notes as rn   # noqa: E402

GOOD = """# Snap Recomp 1.2.3

## Known issues

- Frame generation is off on AMD cards for now.

## Saves and settings carry over

Updating keeps your saves and settings. The updater copies them to a dated folder first and keeps the newest 3 copies.

## Driver notes

- NVIDIA 560 and newer: no known problems.

## Changes

- Fixed the pause pointer at 144 fps ([#41](https://github.com/finalpantasy/new-pokemon-snap-recomp/pull/41))
- Added the Testing channel ([#42](https://github.com/finalpantasy/new-pokemon-snap-recomp/issues/42))

## Install or update

Open the launcher and choose Updates.
"""


class Notes(unittest.TestCase):
    def test_good_notes_pass(self):
        self.assertEqual(rn.lint(GOOD), [])

    def test_fresh_skeleton_is_not_publishable_until_filled(self):
        text = rn.skeleton('1.2.3')
        probs = rn.lint(text)
        self.assertTrue(any('placeholder' in p for p in probs), probs)
        self.assertNotIn('<!--', text)
        self.assertTrue(text.startswith('# Snap Recomp 1.2.3'))

    def test_testing_skeleton_says_so(self):
        text = rn.skeleton('1.3.0-rc1', testing=True)
        self.assertIn('(Testing)', text.splitlines()[0])
        self.assertIn('Stable users are not offered it', text)

    def test_known_issues_come_first(self):
        swapped = GOOD.replace('## Known issues', '## TMP').replace('## Changes', '## Known issues').replace('## TMP', '## Changes')
        self.assertTrue(any('out of order' in p for p in rn.lint(swapped)))

    def test_missing_section(self):
        self.assertTrue(any('missing section "Driver notes"' in p for p in rn.lint(GOOD.replace('## Driver notes', '## Notes'))))

    def test_change_without_link(self):
        bad = GOOD.replace(' ([#41](https://github.com/finalpantasy/new-pokemon-snap-recomp/pull/41))', '')
        self.assertTrue(any('without a pull request' in p for p in rn.lint(bad)))

    def test_link_to_another_repo_does_not_count(self):
        bad = GOOD.replace('finalpantasy/new-pokemon-snap-recomp/pull/41', 'someone/else/pull/41')
        self.assertTrue(any('without a pull request' in p for p in rn.lint(bad)))

    def test_carry_over_must_mention_the_copy(self):
        bad = GOOD.replace('The updater copies them to a dated folder first and keeps the newest 3 copies.', 'Nothing to do.')
        self.assertTrue(any('carry over' in p for p in rn.lint(bad)))

    def test_personal_details_are_refused(self):
        for leak in ('C:' + chr(92) + 'Users' + chr(92) + 'someone' + chr(92) + 'x', 'mail me at a@b.example'):
            self.assertTrue(any('personal' in p for p in rn.lint(GOOD + '\n' + leak + '\n')), leak)

    def test_empty_known_issues(self):
        self.assertTrue(any('Known issues' in p and 'no bullets' in p for p in rn.lint(GOOD.replace('- Frame generation is off on AMD cards for now.', ''))))

    def test_banner_points_at_latest_and_does_not_stack(self):
        b = rn.banner('v1.3.0')
        self.assertIn('v1.3.0', b)
        self.assertIn('/releases/latest', b)
        once = rn.with_banner(GOOD, 'v1.3.0')
        twice = rn.with_banner(once, 'v1.4.0')
        self.assertEqual(twice.count(rn.BANNER_MARK), 1)
        self.assertIn('v1.4.0', twice.splitlines()[0])
        self.assertEqual(rn.lint(twice), [])   # the banner above the title does not break the template

    def test_cli(self):
        import io, contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.assertEqual(rn.main(['new', '--version', '9.9.9']), 0)
        self.assertIn('Snap Recomp 9.9.9', buf.getvalue())


if __name__ == '__main__':
    unittest.main()
