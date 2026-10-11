"""NEXUS.bb is generated from README.md; the converter rejects Markdown it cannot render."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('readme_to_bbcode', ROOT / 'tools/readme_to_bbcode.py')
bbcode = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bbcode)


class ConvertTests(unittest.TestCase):
    def test_spans_headings_lists_and_code(self):
        text = ('# Title\n\nA **bold** and *soft* word, `code` and [a link](https://x.y/z).\n'
                'Wrapped line.\n\n## Part\n\n- one\n  continued\n- two\n\n1. first\n2. second\n\n'
                '```text\nMods/\n  a\n```\n')
        self.assertEqual(bbcode.convert(text), (
            'A [b]bold[/b] and [i]soft[/i] word, [font=Courier New]code[/font] and [url=https://x.y/z]a link[/url]. '
            'Wrapped line.\n\n[size=4][b]Part[/b][/size]\n[list]\n[*]one continued\n[*]two\n[/list]\n\n'
            '[list=1]\n[*]first\n[*]second\n[/list]\n\n[code]Mods/\n  a[/code]\n'))

    def test_relative_links_point_into_the_repository(self):
        out = bbcode.convert('[doc](bridge-files/docs/API.md) [here](#top)', ref='abc')
        self.assertIn('[url=https://github.com/jrpereira/UE4SSLuaEXtensions/blob/abc/bridge-files/docs/API.md]doc[/url]', out)
        self.assertIn(' here', out)

    def test_code_spans_are_not_formatted_further(self):
        self.assertIn('[font=Courier New]a*b*c[/font]', bbcode.convert('`a*b*c`'))

    def test_unsupported_markdown_is_an_error(self):
        for text in ('| a | b |\n| - | - |', '> quote', '![x](y.png)', '#### deep', '```\nopen'):
            with self.assertRaises(ValueError, msg=text):
                bbcode.convert(text)


class GeneratedFileTests(unittest.TestCase):
    def test_nexus_bb_matches_the_readme(self):
        expected = bbcode.convert((ROOT / 'README.md').read_text(encoding='utf-8'))
        self.assertEqual((ROOT / 'NEXUS.bb').read_text(encoding='utf-8'), expected,
                         'NEXUS.bb is out of date; run python3 tools/readme_to_bbcode.py')


if __name__ == '__main__':
    unittest.main()
