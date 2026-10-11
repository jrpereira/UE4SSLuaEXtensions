"""Compiles an extension's native sources against the pinned UE4SS headers with
g++ -fsyntax-only, so ABI and header mistakes show up before the Windows CI
build:

    python3 shared/tests/abi_syntax.py bridge-files

It generates the embedded-Lua header the way shared/cmake/LexProduct.cmake does
and checks every src/*.cpp except those that include <windows.h>, which only
MSVC can compile.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from product_checks import ROOT, SHARED, Extension, resolve  # noqa: E402


def embedded_header(extension, path):
    shared_cmake = (SHARED / 'cmake/LexProduct.cmake').read_text(encoding='utf-8')
    size = int(re.search(r'^set\(LEX_LUA_CHUNK_SIZE (\d+)\)', shared_cmake, re.M)[1])
    source = extension.lua_api.read_bytes()
    delimiter = extension.embed['DELIMITER'].encode()
    chunks = [source[offset:offset + size] for offset in range(0, len(source), size)]
    with open(path, 'wb') as header:
        header.write(b'#pragma once\n#include <array>\n#include <string_view>\n')
        header.write(f'namespace {extension.embed["NAMESPACE"]} {{\n'.encode())
        header.write(f'inline constexpr std::array<std::string_view, {len(chunks)}> embedded_lua_api_chunks{{\n'.encode())
        for chunk in chunks:
            header.write(b'R"' + delimiter + b'(' + chunk + b')' + delimiter + b'",\n')
        header.write(b'};\n}\n')


def main(argv):
    if len(argv) != 2:
        raise SystemExit('usage: abi_syntax.py <extension-folder>')
    extension = Extension(argv[1])
    sources = sorted(path for path in (extension.folder / 'src').glob('*.cpp')
                     if '#include <windows.h>' not in path.read_text(encoding='utf-8'))
    if not sources:
        raise SystemExit(f'{extension.name}: no portable sources to check')
    magic = resolve(extension.cmake, extension.bootstrap['MAGIC'])
    with tempfile.TemporaryDirectory() as generated:
        embedded_header(extension, Path(generated) / 'EmbeddedLuaAPI.hpp')
        command = [
            'g++', '-std=c++20', '-D_WIN32', '-D__declspec(x)=', f'-DUE4SSLEB_IMPLEMENTATION_MAGIC={magic}',
            f'-I{generated}', f'-I{extension.folder / "include"}', f'-I{extension.folder / "contract"}',
            f'-I{SHARED / "contract"}', f'-I{SHARED / "ue4ss-abi"}', f'-I{SHARED / "native"}', f'-I{SHARED}',
            '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-fsyntax-only', *map(str, sources),
        ]
        result = subprocess.run(command, cwd=ROOT)
    names = ', '.join(path.name for path in sources)
    print(f'{extension.name}: ABI syntax {"passed" if result.returncode == 0 else "FAILED"} ({names})')
    return result.returncode


if __name__ == '__main__':
    sys.exit(main(sys.argv))
