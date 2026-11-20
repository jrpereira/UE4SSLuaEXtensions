"""shared/bootstrap/cmake/ModSettings.cmake: mod_settings.ini from mod.json "settings".

Runs the CMake functions in script mode. Without cmake on PATH the tests skip,
except in CI, where they fail.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / 'shared/bootstrap/cmake/ModSettings.cmake'
BRIDGES = sorted(path.parent for path in ROOT.glob('bridge-*/mod.json'))
CMAKE = shutil.which('cmake')
if CMAKE is None and os.environ.get('CI'):
    raise RuntimeError('cmake is required in CI')


@unittest.skipIf(CMAKE is None, 'cmake not found')
class ModSettingsTests(unittest.TestCase):
    def generate(self, manifest):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            source = folder / 'mod.json'
            source.write_text(json.dumps(manifest) if not isinstance(manifest, str) else manifest, encoding='utf-8')
            output = folder / 'mod_settings.ini'
            script = folder / 'run.cmake'
            script.write_text(f'include("{MODULE.as_posix()}")\n'
                              f'lex_write_mod_settings("{source.as_posix()}" "{output.as_posix()}")\n', encoding='utf-8')
            result = subprocess.run([CMAKE, '-P', str(script)], capture_output=True, text=True)
            text = output.read_text(encoding='utf-8') if output.exists() else None
            return result, text

    def test_rows_in_array_order_storage_only_when_given(self):
        result, text = self.generate({
            'id': 'Demo', 'name': 'Demo Mod',
            'settings': [
                {'key': 'Lvl', 'id': 'Level', 'type': 'picker', 'values': [[1, 'Low'], [2, 'Mid'], ['x', 'High']],
                 'default': 2, 'label': 'Level'},
                {'id': 'Alpha', 'type': 'toggle', 'description': 'a; b = c', 'file': 'other.ini', 'section': 'S'},
            ]})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(text,
            '[Mod]\nId=Demo\nName=Demo Mod\n'
            '\n[Setting.Level]\nId=Level\nType=picker\nLabel=Level\nPresetValues=1|2|x\n'
            'PresetLabels=Low|Mid|High\nDefault=2\nConfigKey=Lvl\n'
            '\n[Setting.Alpha]\nId=Alpha\nType=toggle\nDescription=a; b = c\n'
            'ConfigFile=other.ini\nConfigSection=S\n')

    def test_config_section_is_the_pages_default(self):
        result, text = self.generate({'id': 'Demo', 'name': 'Demo', 'config_section': 'Pack.Demo',
                                      'settings': [{'id': 'Flag', 'type': 'toggle'}]})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(text, '[Mod]\nId=Demo\nName=Demo\nConfigSection=Pack.Demo\n'
                               '\n[Setting.Flag]\nId=Flag\nType=toggle\n')

    def test_no_settings_writes_the_mod_section_only(self):
        result, text = self.generate({'id': 'Demo', 'name': 'Demo'})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(text, '[Mod]\nId=Demo\nName=Demo\n')

    def test_refusals(self):
        cases = {
            'not an array': {'settings': {'id': 'A'}},
            'no id': {'settings': [{'type': 'toggle'}]},
            'bad id': {'settings': [{'id': 'a b'}]},
            'bad config_section': {'config_section': 'a b', 'settings': []},
            'repeated id': {'settings': [{'id': 'A'}, {'id': 'A'}]},
            'unknown field': {'settings': [{'id': 'A', 'Label': 'x'}]},
            'values not pairs': {'settings': [{'id': 'A', 'values': [1, 2]}]},
            'pair of three': {'settings': [{'id': 'A', 'values': [[1, 'a', 'b']]}]},
            'bar in a label': {'settings': [{'id': 'A', 'values': [[1, 'x|y']]}]},
            'line break': {'settings': [{'id': 'A', 'label': 'x\ny'}]},
            'object value': {'settings': [{'id': 'A', 'label': {'x': 1}}]},
            'boolean value': {'settings': [{'id': 'A', 'default': True}]},
        }
        for name, extra in cases.items():
            with self.subTest(name=name):
                result, text = self.generate({'id': 'Demo', 'name': 'Demo', **extra})
                self.assertNotEqual(result.returncode, 0)
                self.assertIsNone(text)

    def test_every_bridge_generates(self):
        for bridge in BRIDGES:
            with self.subTest(bridge=bridge.name):
                manifest = json.loads((bridge / 'mod.json').read_text(encoding='utf-8'))
                result, text = self.generate(manifest)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(text.startswith(f"[Mod]\nId={manifest['id']}\nName={manifest['name']}\n"
                                                f"ConfigSection={manifest['config_section']}\n"))
                self.assertIn(f"[Setting.EnableVerifications]\nId=EnableVerifications\nType=toggle\n", text)
                self.assertNotIn('ConfigFile=', text)


if __name__ == '__main__':
    unittest.main()
