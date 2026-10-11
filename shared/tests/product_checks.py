"""Checks every extension must pass, run for one extension folder:

    python3 shared/tests/product_checks.py bridge-files

tests/run.sh calls this before the extension's own suites. The extension's
details come from its own files: the product name from mod.json, the macro
prefix and version from contract/Version.hpp, the Lua API and the embedding
delimiter from the lex_embed_lua call in CMakeLists.txt, and the bootstrap
identity from its add_bridge_bootstrap call.
"""
import importlib.util
import json
from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
SHARED = ROOT / 'shared'


def _load_release_notes():
    spec = importlib.util.spec_from_file_location('release_notes', ROOT / 'tools/release_notes.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def cmake_call(text, function):
    """The first `function(...)` call: its first word and {KEYWORD: value}."""
    match = re.search(r'^\s*' + re.escape(function) + r'\(', text, re.M)
    if not match:
        raise ValueError(f'no {function}() call')
    index, depth, quoted = match.end(), 1, False
    while depth:
        char = text[index]
        if char == '"':
            quoted = not quoted
        elif not quoted and char == '(':
            depth += 1
        elif not quoted and char == ')':
            depth -= 1
        index += 1
    tokens = [token.strip('"') for token in re.findall(r'"[^"]*"|[^\s"]+', text[match.end():index - 1])]
    first, arguments, key = (tokens[0] if tokens else None), {}, None
    for token in tokens:
        if re.fullmatch(r'[A-Z][A-Z_]+', token):
            key = token
            arguments[key] = ''
        elif key:
            arguments[key] = (arguments[key] + ' ' + token).strip()
    return first, arguments


def resolve(text, value):
    """Expands ${NAME} from a set(NAME value) in the same CMakeLists.txt."""
    def lookup(match):
        found = re.search(r'^set\(' + re.escape(match[1]) + r' ([^)\s]+)\)', text, re.M)
        if not found:
            raise ValueError('unresolved ${%s}' % match[1])
        return found[1]
    return re.sub(r'\$\{([A-Za-z0-9_]+)\}', lookup, value)


class Extension:
    def __init__(self, folder):
        self.folder = (ROOT / folder).resolve()
        if not (self.folder / 'CMakeLists.txt').is_file():
            raise SystemExit(f'{folder}: not an extension folder (no CMakeLists.txt)')
        self.name = self.folder.name
        self.cmake = (self.folder / 'CMakeLists.txt').read_text(encoding='utf-8')
        self.manifest = json.loads((self.folder / 'mod.json').read_text(encoding='utf-8'))
        self.product = self.manifest['id']
        header = (self.folder / 'contract/Version.hpp').read_text(encoding='utf-8')
        versions = re.findall(r'^#define ([A-Z0-9_]+)_VERSION "(\d+)\.(\d+)\.(\d+)"$', header, re.M)
        if len(versions) != 1:
            raise SystemExit(f'{folder}: contract/Version.hpp needs exactly one #define <PREFIX>_VERSION "x.y.z"')
        self.prefix, *parts = versions[0]
        self.version_header = header
        self.version_parts = parts
        self.version = '.'.join(parts)
        _, self.embed = cmake_call(self.cmake, 'lex_embed_lua')
        _, self.bootstrap = cmake_call(self.cmake, 'add_bridge_bootstrap')
        self.lua_api = Path(self.embed['SOURCE'].replace('${CMAKE_CURRENT_SOURCE_DIR}', str(self.folder)))


EXTENSION = None


class ProductChecks(unittest.TestCase):
    def test_version_header_parts_agree(self):
        major, minor, patch = EXTENSION.version_parts
        for part, value in (('MAJOR', major), ('MINOR', minor), ('PATCH', patch)):
            self.assertRegex(EXTENSION.version_header, rf'(?m)^#define {EXTENSION.prefix}_VERSION_{part} {value}$')

    def test_embedded_lua_fits_msvc_literals(self):
        shared_cmake = (SHARED / 'cmake/LexProduct.cmake').read_text(encoding='utf-8')
        size = re.search(r'^set\(LEX_LUA_CHUNK_SIZE (\d+)\)', shared_cmake, re.M)
        self.assertIsNotNone(size)
        # MSVC rejects string literals over about 16 KB; keep a wide margin.
        self.assertLessEqual(int(size[1]), 7000)
        self.assertTrue(EXTENSION.lua_api.is_file(), EXTENSION.lua_api)
        self.assertEqual(EXTENSION.embed['NAMESPACE'], EXTENSION.product)
        source = EXTENSION.lua_api.read_text(encoding='utf-8')
        self.assertNotIn(')' + EXTENSION.embed['DELIMITER'], source)

    def test_resource_and_selector_carry_the_version(self):
        major, minor, patch = EXTENSION.version_parts
        version = EXTENSION.version
        template = (SHARED / 'templates/Implementation.rc.in').read_text(encoding='utf-8')
        rendered = (template.replace('@LEX_VERSION_MAJOR@', major).replace('@LEX_VERSION_MINOR@', minor)
                    .replace('@LEX_VERSION_PATCH@', patch).replace('@LEX_VERSION@', version)
                    .replace('@LEX_PRODUCT@', EXTENSION.product)
                    .replace('@LEX_DISPLAY_NAME@', EXTENSION.bootstrap['DISPLAY_NAME']))
        self.assertNotIn('@', rendered)
        self.assertIn(f'FILEVERSION {major},{minor},{patch},0', rendered)
        self.assertIn(f'VALUE "ProductName", "{EXTENSION.product}\\0"', rendered)
        self.assertIn(f'VALUE "ProductVersion", "{version}\\0"', rendered)
        self.assertIn(f'VALUE "OriginalFilename", "{EXTENSION.product}-{version}.dll\\0"', rendered)
        _, resource = cmake_call(EXTENSION.cmake, 'lex_implementation_resource')
        self.assertEqual(resource['PRODUCT'], EXTENSION.product)
        self.assertIn(f'{EXTENSION.product}-${{', EXTENSION.cmake, 'implementation DLL output name')

        selector = (SHARED / 'templates/main.json.in').read_text(encoding='utf-8')
        self.assertEqual(json.loads(selector.replace('@LEX_VERSION@', version)), {'schema': 1, 'version': version})

    def test_bootstrap_identity_matches_the_product(self):
        identity = EXTENSION.bootstrap
        self.assertEqual(identity['PRODUCT'], EXTENSION.product)
        self.assertEqual(identity['DIST_FOLDER'], '0_ModCore_' + EXTENSION.product)
        magic = resolve(EXTENSION.cmake, identity['MAGIC'])
        self.assertRegex(magic, r'^0x[0-9A-Fa-f]{8}$')
        for key in ('TARGET', 'CLAIM_NAME', 'DISPLAY_NAME', 'VERSION_FILE', 'SELECTOR_FILE', 'ENABLED_FILE'):
            self.assertIn(key, identity)

    def test_mod_folder_ships_its_verifications(self):
        identity = EXTENSION.bootstrap
        script = Path(identity['SCRIPT_FILE'].replace('${CMAKE_CURRENT_SOURCE_DIR}', str(EXTENSION.folder)))
        self.assertTrue(script.is_file(), script)
        self.assertIn('${LEX_SHARED_DIR}/lua/lex_verify.lua', identity.get('SCRIPT_MODULES', ''))
        self.assertIn('require("lex_verify")', script.read_text(encoding='utf-8'))
        settings = (EXTENSION.folder / 'packaging/mod_settings.ini').read_text(encoding='utf-8')
        self.assertIn(f'Id={EXTENSION.product}\n', settings)
        self.assertIn('ConfigSection=Verifications\nConfigKey=LoadTime\n', settings)
        self.assertIn('Default=0\n', settings)

    def test_changelog_sections_are_extractable(self):
        release_notes = _load_release_notes()
        text = (EXTENSION.folder / 'CHANGELOG.md').read_text(encoding='utf-8')
        headings = re.findall(r'^## (.+?)\s*$', text, re.M)
        self.assertTrue(headings)
        for index, heading in enumerate(headings):
            with self.subTest(heading=heading):
                if heading == 'Unreleased':
                    self.assertEqual(index, 0, '"## Unreleased" must be the first section')
                    continue
                self.assertRegex(heading, r'^v\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$')
                self.assertTrue(release_notes.extract_notes(text, heading).strip())
        self.assertIn('v' + EXTENSION.version, headings, 'the current version needs a changelog section')


def main(argv):
    global EXTENSION
    if len(argv) < 2:
        raise SystemExit('usage: product_checks.py <extension-folder> [unittest options]')
    EXTENSION = Extension(argv[1])
    program = unittest.main(argv=[f'product_checks {EXTENSION.name}'] + argv[2:], exit=False)
    return 0 if program.result.wasSuccessful() else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
