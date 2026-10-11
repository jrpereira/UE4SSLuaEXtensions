"""Ensure release notes cannot accidentally include another version's history.
Each extension's CHANGELOG.md is checked by shared/tests/product_checks.py."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('release_notes', ROOT / 'tools/release_notes.py')
release_notes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release_notes)
extract_notes = release_notes.extract_notes


class ReleaseNotesTests(unittest.TestCase):
    def test_exact_version_and_subsections(self):
        text = '# Changelog\n\n## v1.0.10\n\nNewer.\n\n## v1.0.1\n\n### Changes\n\nChosen.\n\n## v1.0.0\n\nOlder.\n'
        self.assertEqual(extract_notes(text, 'v1.0.1'), '### Changes\n\nChosen.\n')

    def test_reject_missing_duplicate_empty_and_invalid_tag(self):
        for text, tag in [
            ('## v1.0.10\nOther', 'v1.0.1'),
            ('## v1.0.1\nOne\n## v1.0.1\nTwo', 'v1.0.1'),
            ('## v1.0.1\n\n## v1.0.0\nOlder', 'v1.0.1'),
            ('## latest\nNotes', 'latest'),
        ]:
            with self.subTest(text=text, tag=tag), self.assertRaises(ValueError):
                extract_notes(text, tag)

    def test_code_headings_do_not_end_version(self):
        text = '## v1.0.1\nExample:\n```md\n## v9.0.0\n```\nKept.\n## v1.0.0\nOlder.'
        self.assertIn('Kept.', extract_notes(text, 'v1.0.1'))
        self.assertNotIn('Older.', extract_notes(text, 'v1.0.1'))

    def test_historical_prerelease_and_crlf(self):
        self.assertEqual(extract_notes('## v0.3.4-rc.3\r\n\r\nHistorical.\r\n', 'v0.3.4-rc.3'), 'Historical.\n')


if __name__ == '__main__':
    unittest.main()
