import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import native_candidate as native
from release_session import inspect_candidate


class NativeCandidateTests(unittest.TestCase):
    def fixture(self, root):
        (root/'bridge-events/contract').mkdir(parents=True)
        (root/'bridge-events/contract/Version.hpp').write_text('#define UE4SSLEB_VERSION "1.2.3"')
        for directory in ['shared', 'tests']:
            (root/directory).mkdir()
        (root/'CMakeLists.txt').write_text('fixture')
        lua = root/'build/tools/lua-5.4.8/src/lua.exe'
        lua.parent.mkdir(parents=True)
        lua.write_bytes(b'fake runtime for command-runner tests')
        return root/'build/candidate'

    def runner(self, output):
        def run(command, **kwargs):
            if command[:3] == ['cmake','--build',str(output)]:
                staged = output/'dist/0_ModCore_UE4SSLuaEventBridge'
                (staged/'dlls/versions').mkdir(parents=True)
                (staged/'dlls/main.dll').write_bytes(b'fixture dll')
                (staged/'dlls/main.json').write_text('{"schema":1,"version":"1.2.3"}')
                (staged/'dlls/versions/UE4SSLuaEventBridge-1.2.3.dll').write_bytes(b'fixture implementation')
                (staged/'enabled.txt').write_bytes(b'')
        return run

    def test_dirty_same_version_source_and_new_headers_are_detected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output = self.fixture(root)
            path = native.build(root, output, self.runner(output))
            manifest = json.loads(path.read_text())
            self.assertEqual(native.validate(manifest,root), [])
            (root/'tests/__pycache__').mkdir(parents=True)
            (root/'tests/__pycache__/cached.pyc').write_bytes(b'generated cache')
            self.assertEqual(native.validate(manifest,root), [])
            staged = output/'dist/0_ModCore_UE4SSLuaEventBridge'
            result = inspect_candidate(manifest,staged,staged,root)
            self.assertFalse(result['needs_attention'])
            (root/'bridge-events/new.hpp').write_text('new header')
            result = inspect_candidate(manifest,staged,staged,root)
            self.assertTrue(result['needs_attention'])
            self.assertEqual(result['working_differences'], ['bridge-events/new.hpp'])
            self.assertFalse(result['staged_version_differs_from_working'])
            with self.assertRaises(ValueError):
                native.build(root, output, self.runner(output))

    def test_failed_tests_never_certify_candidate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output = self.fixture(root)
            def run(command, **kwargs):
                self.runner(output)(command, **kwargs)
                if command[0] == 'ctest':
                    raise subprocess.CalledProcessError(1, command)
            with self.assertRaises(subprocess.CalledProcessError):
                native.build(root,output,run)
            self.assertFalse((output/'candidate-manifest.json').exists())

    def test_debug_native_and_both_lua_suites_are_required(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output = self.fixture(root)
            commands = []
            def run(command, **kwargs):
                self.assertEqual(kwargs, {'check': True, 'cwd': root})
                commands.append(command)
                self.runner(output)(command, **kwargs)
            manifest = json.loads(native.build(root, output, run).read_text())
            self.assertIn('-DUE4SSLEB_BUILD_TESTS=OFF', commands[0])
            self.assertIn('-DCMAKE_BUILD_TYPE=Debug', commands[2])
            self.assertIn('--no-tests=error', commands[4])
            self.assertEqual([c[1] for c in commands[-2:]], native.LUA_TEST_SUITES)
            self.assertEqual(native.validate(manifest, root), [])
            del manifest['native_provenance']['test_configuration']
            with self.assertRaisesRegex(ValueError, 'assertion-enabled'):
                native.validate(manifest, root)

    def test_each_lua_failure_prevents_certification(self):
        for suite in native.LUA_TEST_SUITES:
            with self.subTest(suite=suite), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                output = self.fixture(root)
                def run(command, **kwargs):
                    self.runner(output)(command, **kwargs)
                    if command[-1] == suite:
                        raise subprocess.CalledProcessError(1, command)
                with self.assertRaises(subprocess.CalledProcessError):
                    native.build(root, output, run)
                self.assertFalse((output/'candidate-manifest.json').exists())

    def test_source_edit_during_build_never_certifies_candidate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output = self.fixture(root)
            def run(command, **kwargs):
                self.runner(output)(command, **kwargs)
                if command[0] == 'ctest':
                    (root/'bridge-events/edited.cpp').write_text('changed during build')
            with self.assertRaises(RuntimeError):
                native.build(root,output,run)
            self.assertFalse((output/'candidate-manifest.json').exists())


if __name__ == '__main__':
    unittest.main()
