"""Build and identify a local bridge DLL from a fresh, tested CMake build."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

from deployment_preflight import digest


def sources(root):
    paths = [root/'CMakeLists.txt']
    for directory in ['bridge-events', 'shared', 'tests']:
        paths.extend(p for p in (root/directory).rglob('*') if p.is_file()
                     and '__pycache__' not in p.parts and p.suffix not in {'.pyc','.pyo'})
    result = {}
    for path in paths:
        if path.is_symlink() or not path.resolve().is_relative_to(root.resolve()):
            raise ValueError('Native source must reside within the project')
        result[path.relative_to(root).as_posix()] = digest(path)
    if any(value is None for value in result.values()):
        raise ValueError('Missing native build source')
    return dict(sorted(result.items()))


def version(root):
    text = (root/'bridge-events/contract/Version.hpp').read_text()
    match = re.search(r'^#define UE4SSLEB_VERSION "([^"]+)"', text, re.M)
    if not match:
        raise ValueError('Bridge version missing')
    return match[1]


NATIVE_TEST_TARGETS = [
    'SessionAliasIndexTests', 'BindingSnapshotTests', 'WeakObjectPtrTests',
    'QueueBuffersTests', 'QueueDispatchScheduleTests', 'DispatchBudgetTests',
    'NativeBackendLifecycleTests', 'BootstrapSelectionTests',
]
LUA_TEST_SUITES = ['bridge-events/tests/LuaHelperTests.lua', 'bridge-events/tests/LifecycleIntegrationTests.lua']


def find_lua(root, executable=None):
    candidate = executable or root/'build/tools/lua-5.4.8/src/lua.exe'
    if Path(candidate).is_file():
        return str(Path(candidate).resolve())
    located = shutil.which(str(executable)) if executable else (shutil.which('lua5.4') or shutil.which('lua'))
    if not located:
        raise ValueError('Lua 5.4 is required; supply --lua or build the local Lua runtime first')
    return located


def build(root, output, runner=subprocess.run, lua_executable=None):
    root, output = root.resolve(), output.resolve()
    if output.exists():
        raise ValueError('Use a new output directory; never certify an existing DLL')
    lua = find_lua(root, lua_executable)
    before = sources(root)
    value = version(root)
    output.mkdir(parents=True)
    test_output = output/'native-tests'
    commands = [
        ['cmake','-S',str(root),'-B',str(output),'-G','Ninja',
         '-DCMAKE_BUILD_TYPE=Release','-DUE4SSLEB_BUILD_TESTS=OFF',
         '-DUE4SSLEB_COMPONENT_LAYOUT_128=OFF'],
        ['cmake','--build',str(output),'--config','Release'],
        ['cmake','-S',str(root),'-B',str(test_output),'-G','Ninja',
         '-DCMAKE_BUILD_TYPE=Debug','-DUE4SSLEB_BUILD_TESTS=ON',
         '-DUE4SSLEB_COMPONENT_LAYOUT_128=OFF'],
        ['cmake','--build',str(test_output),'--config','Debug','--target',*NATIVE_TEST_TARGETS],
        ['ctest','--test-dir',str(test_output),'--output-on-failure','--no-tests=error'],
        [lua,'-e',"assert(_VERSION == 'Lua 5.4', 'Lua 5.4 is required')"],
        *[[lua, suite] for suite in LUA_TEST_SUITES],
    ]
    for command in commands:
        runner(command, check=True, cwd=root)
    if sources(root) != before:
        raise RuntimeError('Source changed during build; no candidate manifest produced')
    staged = output/'dist/0_ModCore_UE4SSLuaEventBridge'
    paths = ['dlls/main.dll', 'dlls/main.json',
             f'dlls/versions/UE4SSLuaEventBridge-{value}.dll', 'enabled.txt']
    files = {p:digest(staged/p) for p in paths}
    if any(value is None for value in files.values()):
        raise RuntimeError('Native build did not produce the complete Windows DLL payload')
    manifest = {'module':'0_ModCore_UE4SSLuaEventBridge','version':value,'files':files,
                'native_provenance':{'source_files':before,'configuration':'Release',
                                     'experimental_component_layout':False,'tests_passed':True,
                                     'test_configuration':'Debug',
                                     'native_test_targets':NATIVE_TEST_TARGETS,
                                     'lua_test_suites':LUA_TEST_SUITES}}
    path = output/'candidate-manifest.json'
    path.write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    return path


def validate(manifest, root):
    provenance = manifest.get('native_provenance', {})
    value = manifest.get('version')
    expected = {'dlls/main.dll', 'dlls/main.json',
                f'dlls/versions/UE4SSLuaEventBridge-{value}.dll', 'enabled.txt'}
    if manifest.get('module') != '0_ModCore_UE4SSLuaEventBridge' or set(manifest.get('files',{})) != expected:
        raise ValueError('Unexpected native candidate payload')
    if provenance.get('configuration') != 'Release' or provenance.get('experimental_component_layout') is not False or provenance.get('tests_passed') is not True:
        raise ValueError('Candidate lacks a verified production build record; rebuild with native_candidate.py')
    if (provenance.get('test_configuration') != 'Debug'
            or provenance.get('native_test_targets') != NATIVE_TEST_TARGETS
            or provenance.get('lua_test_suites') != LUA_TEST_SUITES):
        raise ValueError('Candidate lacks assertion-enabled native and Lua test evidence; rebuild with native_candidate.py')
    captured, current = provenance.get('source_files', {}), sources(root)
    return sorted(path for path in set(captured)|set(current) if captured.get(path) != current.get(path))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--working', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--lua', help='Lua 5.4 executable; defaults to the local runtime or PATH')
    args = parser.parse_args()
    print(build(args.working, args.output, lua_executable=args.lua))


if __name__ == '__main__':
    main()
