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

    def test_rows_in_array_order_with_keys_in_the_usual_order(self):
        result, text = self.generate({
            'id': 'Demo', 'name': 'Demo Mod',
            'settings': [
                {'ConfigKey': 'Level', 'Id': 'Level', 'Type': 'picker', 'PresetValues': [1, 2, 3],
                 'PresetLabels': ['Low', 'Mid', 'High'], 'Default': 2, 'Step': '1', 'Label': 'Level'},
                {'Id': 'Alpha', 'Type': 'toggle', 'Description': 'a; b = c'},
            ]})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(text,
            '[Mod]\nId=Demo\nName=Demo Mod\n'
            '\n[Setting.Level]\nId=Level\nType=picker\nLabel=Level\nPresetValues=1|2|3\n'
            'PresetLabels=Low|Mid|High\nDefault=2\nConfigKey=Level\nStep=1\n'
            '\n[Setting.Alpha]\nId=Alpha\nType=toggle\nDescription=a; b = c\n')

    def test_no_settings_writes_the_mod_section_only(self):
        result, text = self.generate({'id': 'Demo', 'name': 'Demo'})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(text, '[Mod]\nId=Demo\nName=Demo\n')

    def test_refusals(self):
        cases = {
            'not an array': {'settings': {'Id': 'A'}},
            'no Id': {'settings': [{'Type': 'toggle'}]},
            'bad Id': {'settings': [{'Id': 'a b'}]},
            'repeated Id': {'settings': [{'Id': 'A'}, {'Id': 'A'}]},
            'bar in an item': {'settings': [{'Id': 'A', 'PresetLabels': ['x|y']}]},
            'line break': {'settings': [{'Id': 'A', 'Label': 'x\ny'}]},
            'object value': {'settings': [{'Id': 'A', 'Label': {'x': 1}}]},
            'boolean value': {'settings': [{'Id': 'A', 'Default': True}]},
            'bad key': {'settings': [{'Id': 'A', 'Config-Key': 'x'}]},
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
                self.assertTrue(text.startswith(f"[Mod]\nId={manifest['id']}\n"))
                self.assertIn('[Setting.EnableVerifications]\n', text)


if __name__ == '__main__':
    unittest.main()
