"""The combined Nexus package is described by package-manifest.json: each extension's mod folder
keeps its verified bytes, its documents go in docs/ inside that folder, and nothing else ships."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_collection', ROOT / 'tools/build_collection.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)

MANIFEST = build.load_manifest()
COMMIT = 'a' * 40
STAMP = (2026, 10, 11, 12, 0, 0)
EVENTS = MANIFEST['extensions'][0]
FILES = MANIFEST['extensions'][1]


def product_release(extension, version, tamper=None):
    name, mod = extension['name'], extension['mod_folder'] + '/'
    payload = {mod + 'enabled.txt': b'1', mod + 'dlls/main.dll': b'MZ-' + name.encode()}
    described = {'schema': 1, 'product': name, 'version': version, 'source_commit': COMMIT,
                 'files': [{'path': p, 'sha256': hashlib.sha256(d).hexdigest()} for p, d in payload.items()]}
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w') as z:
        for path, data in payload.items():
            z.writestr(zipfile.ZipInfo(path, STAMP), data)
        if tamper == 'extra':
            z.writestr(zipfile.ZipInfo(mod + 'tools/build.ps1', STAMP), b'x')
    archive = buffer.getvalue()
    checksum = f'{hashlib.sha256(archive).hexdigest()}  {name}-v{version}.zip\n'
    product = {'product_name': name, 'version': version, 'tag': f'{extension["folder"]}/v{version}', 'commit': COMMIT,
               'archive': f'{name}-v{version}.zip', 'mod_folder': extension['mod_folder']}
    return product, archive, checksum, json.dumps(described).encode()


REPO = {
    'LICENSE': b'MIT\n',
    'README.md': b'root readme\n',
    'bridge-x/README.md': b'# X\n- [Guide](docs/DEVELOPERS.md#a)\n- [Maintainer guide](docs/BUILD.md): build.\n'
                          b'- [Sample](examples/A/Scripts/main.lua)\n- [Source](src/a.cpp)\n- [Changes](CHANGELOG.md)\n',
    'bridge-x/CHANGELOG.md': b'# Changelog\n',
    'bridge-x/docs/DEVELOPERS.md': b'# Dev\nSee the [maintainer guide](BUILD.md#t).\n[Back](../README.md)\nKept.\n',
    'bridge-x/docs/BUILD.md': b'# Build\n',
    'bridge-x/docs/SPEC-REVIEW.md': b'# Review\n',
    'bridge-x/docs/API.md': (b'# API\n\nStatus: **frozen.** Process note\nacross two lines.\n\nIntro stays.\n\n'
                             b'- [Used](#used)\n- [Internal native contract](#internal-native-contract)\n\n'
                             b'## Used\n\nUsage.\n\n## Internal native contract\n\nSecret.\n\n## Open points for review\n\n- p\n'),
    'bridge-x/examples/A/Scripts/main.lua': b'print(1)\n',
    'bridge-x/examples/A/enabled.txt': b'1',
    'bridge-x/src/a.cpp': b'int x;\n',
}
X = {'name': 'X', 'folder': 'bridge-x', 'mod_folder': '0_ModCore_X', 'documents': FILES['documents']}


def documents(extension=X):
    def list_files(path):
        if not path:
            return [name for name in REPO if '/' not in name]
        return [name for name in REPO if name.startswith(path + '/')]

    return build.product_documents(extension, 'bridge-x/v1.0.0', MANIFEST, REPO.get, list_files)


class ManifestTests(unittest.TestCase):
    def test_the_manifest_describes_both_extensions_and_what_is_not_deployed(self):
        self.assertEqual([e['mod_folder'] for e in MANIFEST['extensions']],
                         ['0_ModCore_UE4SSLuaEventBridge', '0_ModCore_UE4SSLuaFileBridge'])
        self.assertEqual(MANIFEST['not_deployed'], ['NEXUS.bb', 'DEVELOPMENT.md'])
        self.assertEqual(MANIFEST['nexus_description'], {'source': 'README.md', 'output': 'NEXUS.bb'})

    def test_the_menu_folder_lists_files_that_exist_and_no_development_folders(self):
        for folder in MANIFEST['folders']:
            self.assertEqual(folder['mod_folder'], f'0_ModCore_{folder["name"]}')
            for path in folder['files']:
                self.assertTrue((ROOT / folder['source'] / path).is_file(), path)
                self.assertFalse({p.lower() for p in path.split('/')[:-1]} & {'tools', 'tests'}, path)

    def test_every_extension_ships_a_license_inside_its_docs(self):
        for extension in MANIFEST['extensions']:
            self.assertIn({'from': '/LICENSE', 'to': 'docs/LICENSE'}, extension['documents'])

    def test_only_the_root_may_hold_a_nexus_description(self):
        tracked = subprocess.run(['git', 'ls-files'], cwd=ROOT, capture_output=True, text=True).stdout.split()
        self.assertEqual([p for p in tracked if p.lower().endswith('nexus.bb')], ['NEXUS.bb'])


class VerifyProductTests(unittest.TestCase):
    def test_accepts_a_matching_release(self):
        product, archive, checksum, described = product_release(EVENTS, '1.0.13')
        entries, digest = build.verify_product(product, archive, checksum, described, COMMIT, MANIFEST)
        self.assertEqual(set(entries), {'0_ModCore_UE4SSLuaEventBridge/enabled.txt',
                                        '0_ModCore_UE4SSLuaEventBridge/dlls/main.dll'})
        self.assertEqual(digest, hashlib.sha256(archive).hexdigest())

    def test_rejects_a_wrong_checksum_a_foreign_commit_and_a_tools_folder(self):
        product, archive, checksum, described = product_release(EVENTS, '1.0.13')
        with self.assertRaisesRegex(ValueError, 'checksum'):
            build.verify_product(product, archive, '0' * 64 + '  x.zip\n', described, COMMIT, MANIFEST)
        with self.assertRaisesRegex(ValueError, 'commit'):
            build.verify_product(product, archive, checksum, described, 'b' * 40, MANIFEST)
        product, archive, checksum, described = product_release(FILES, '1.0.1', tamper='extra')
        with self.assertRaisesRegex(ValueError, 'tools or tests'):
            build.verify_product(product, archive, checksum, described, COMMIT, MANIFEST)


class DocumentTests(unittest.TestCase):
    def test_documents_land_in_the_mod_folders_docs(self):
        self.assertEqual(sorted(documents()), ['docs/API.md', 'docs/DEVELOPERS.md', 'docs/LICENSE', 'docs/README.md',
                                               'docs/examples/A/Scripts/main.lua'])
        self.assertEqual(documents()['docs/LICENSE'], b'MIT\n')

    def test_links_follow_the_files_and_unshipped_ones_point_at_the_source(self):
        readme = documents()['docs/README.md'].decode()
        self.assertIn('(DEVELOPERS.md#a)', readme)
        self.assertIn('(examples/A/Scripts/main.lua)', readme)
        self.assertIn('(https://github.com/jrpereira/UE4SSLuaEXtensions/blob/bridge-x/v1.0.0/bridge-x/src/a.cpp)', readme)
        self.assertIn('(https://github.com/jrpereira/UE4SSLuaEXtensions/blob/bridge-x/v1.0.0/bridge-x/CHANGELOG.md)', readme)
        self.assertIn('(README.md)', documents()['docs/DEVELOPERS.md'].decode())

    def test_development_text_is_dropped(self):
        api = documents()['docs/API.md'].decode()
        self.assertIn('Intro stays.', api)
        self.assertIn('Usage.', api)
        for dropped in ('Status:', 'Process note', 'Secret', 'Internal native', 'Open points', 'across two lines'):
            self.assertNotIn(dropped, api)
        for name, data in documents().items():
            for dropped in ('BUILD.md', 'SPEC-REVIEW.md', 'Maintainer'):
                self.assertNotIn(dropped, data.decode(), name)

    def test_a_rule_that_matches_nothing_is_an_error(self):
        broken = dict(X, documents=[{'from': 'guides/*.md', 'to': 'docs/'}])
        with self.assertRaisesRegex(ValueError, 'matches nothing'):
            documents(broken)


class PackageFolderTests(unittest.TestCase):
    def test_the_menu_folder_takes_the_package_version_in_its_mod_json(self):
        folders = build.package_folders(MANIFEST, '1.1.0', lambda path: (ROOT / path).read_bytes())
        self.assertEqual([f['mod_folder'] for f in folders], ['0_ModCore_UE4SSLuaEXtensions'])
        described = json.loads(folders[0]['files']['mod.json'])
        self.assertEqual(described['version'], '1.1.0')
        self.assertEqual(list(described)[:3], ['manifest_version', 'id', 'name'])
        self.assertNotIn('version_source', described)
        self.assertIn('Scripts/main.lua', folders[0]['files'])

    def test_the_root_readme_lands_in_the_menu_folders_docs_with_links_to_the_extensions(self):
        targets = {'bridge-events/README.md': '0_ModCore_UE4SSLuaEventBridge/docs/README.md',
                   'bridge-events/docs/DEVELOPERS.md': '0_ModCore_UE4SSLuaEventBridge/docs/DEVELOPERS.md',
                   'bridge-files/docs/API.md': '0_ModCore_UE4SSLuaFileBridge/docs/API.md'}
        folders = build.package_folders(MANIFEST, '1.1.0', lambda path: (ROOT / path).read_bytes(), targets, 'd' * 40)
        readme = folders[0]['files']['docs/README.md'].decode()
        self.assertIn('(../../0_ModCore_UE4SSLuaEventBridge/docs/README.md)', readme)
        self.assertIn('(../../0_ModCore_UE4SSLuaFileBridge/docs/API.md)', readme)
        self.assertIn('(https://github.com/jrpereira/UE4SSLuaEXtensions/blob/' + 'd' * 40 + '/bridge-files/README.md)', readme)
        self.assertIn('(https://github.com/jrpereira/UE4SSLuaEXtensions/releases)', readme)

    def test_a_missing_file_is_an_error(self):
        with self.assertRaisesRegex(ValueError, 'missing'):
            build.package_folders(MANIFEST, '1.1.0', lambda path: None)


class AssembleTests(unittest.TestCase):
    def setUp(self):
        self.products = []
        for extension, version in ((EVENTS, '1.0.13'), (FILES, '1.0.1')):
            product, archive, checksum, described = product_release(extension, version)
            entries, digest = build.verify_product(product, archive, checksum, described, COMMIT, MANIFEST)
            product.update(entries=entries, archive_sha256=digest,
                           docs={'docs/README.md': b'readme', 'docs/LICENSE': b'MIT\n'})
            self.products.append(product)

    def build(self, directory, version='1.1.0'):
        folders = build.package_folders(MANIFEST, version, lambda path: (ROOT / path).read_bytes()) \
            if build.VERSION.fullmatch(version) else []
        return build.assemble(MANIFEST, version, 'c' * 40, self.products, directory, STAMP, folders)

    def test_the_package_has_only_the_mod_folders_and_no_root_files(self):
        with tempfile.TemporaryDirectory() as directory:
            target = self.build(directory)
            self.assertEqual(target.name, 'UE4SSLuaEXtensions-v1.1.0.zip')
            with zipfile.ZipFile(target) as z:
                names = z.namelist()
                self.assertEqual({n.split('/')[0] for n in names},
                                 {'0_ModCore_UE4SSLuaEventBridge', '0_ModCore_UE4SSLuaFileBridge',
                                  '0_ModCore_UE4SSLuaEXtensions'})
                self.assertIn('0_ModCore_UE4SSLuaEXtensions/Scripts/extensions_menu.lua', names)
                self.assertIn('0_ModCore_UE4SSLuaEXtensions/docs/README.md', names)
                self.assertEqual([n for n in names if '/' not in n], [])
                self.assertIn('0_ModCore_UE4SSLuaFileBridge/docs/LICENSE', names)
                self.assertFalse([n for n in names if n.endswith('NEXUS.bb') or n == 'README.md'])
                for name in names:
                    self.assertFalse({p.lower() for p in name.split('/')[:-1]} & {'tools', 'tests'}, name)
                for product in self.products:
                    for name, (_, data) in product['entries'].items():
                        self.assertEqual(z.read(name), data)

    def test_checksum_and_manifest_describe_the_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            target = self.build(directory)
            self.assertEqual(Path(f'{target}.sha256').read_text().split()[0], hashlib.sha256(target.read_bytes()).hexdigest())
            described = json.loads(Path(f'{target}.manifest.json').read_text())
            self.assertEqual([p['tag'] for p in described['products']], ['bridge-events/v1.0.13', 'bridge-files/v1.0.1'])
            with zipfile.ZipFile(target) as z:
                self.assertEqual({f['path'] for f in described['files']}, set(z.namelist()))

    def test_output_is_deterministic_and_never_overwritten(self):
        with tempfile.TemporaryDirectory() as one, tempfile.TemporaryDirectory() as two:
            self.assertEqual(self.build(one).read_bytes(), self.build(two).read_bytes())
            with self.assertRaises(FileExistsError):
                self.build(one)
            with self.assertRaises(ValueError):
                self.build(two, version='1.1')

    def test_a_tools_folder_in_the_documents_is_rejected(self):
        self.products[0]['docs']['docs/tests/notes.md'] = b'x'
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, 'tools or tests'):
                self.build(directory)


if __name__ == '__main__':
    unittest.main()
