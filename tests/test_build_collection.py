"""The combined Nexus package keeps each extension's folder intact and puts documentation outside it."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_collection', ROOT / 'tools/build_collection.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)

COMMIT = 'a' * 40
STAMP = (2026, 10, 11, 12, 0, 0)


def product_release(name, version, folder, commit=COMMIT, tamper=None):
    mod = f'0_ModCore_{name}/'
    payload = {mod + 'enabled.txt': b'1', mod + 'dlls/main.dll': b'MZ-' + name.encode()}
    manifest = {'schema': 1, 'product': name, 'version': version, 'source_commit': commit,
                'files': [{'path': path, 'sha256': hashlib.sha256(data).hexdigest()} for path, data in payload.items()]}
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w') as z:
        for path, data in payload.items():
            z.writestr(zipfile.ZipInfo(path, STAMP), data)
        if tamper == 'extra':
            z.writestr(zipfile.ZipInfo(mod + 'tools/build.ps1', STAMP), b'x')
    archive = buffer.getvalue()
    checksum = f'{hashlib.sha256(archive).hexdigest()}  {name}-v{version}.zip\n'
    return {'product_name': name, 'version': version, 'tag': f'{folder}/v{version}', 'commit': COMMIT,
            'archive': f'{name}-v{version}.zip', 'folder_in_repo': folder}, archive, checksum, json.dumps(manifest).encode()


def verified(name, version, folder, docs):
    product, archive, checksum, manifest = product_release(name, version, folder)
    entries, digest = build.verify_product(product, archive, checksum, manifest, COMMIT)
    product.update(entries=entries, archive_sha256=digest, docs=docs)
    return product


class VerifyProductTests(unittest.TestCase):
    def test_accepts_a_matching_release(self):
        product, archive, checksum, manifest = product_release('UE4SSLuaEventBridge', '1.0.13', 'bridge-events')
        entries, digest = build.verify_product(product, archive, checksum, manifest, COMMIT)
        self.assertEqual(set(entries), {'0_ModCore_UE4SSLuaEventBridge/enabled.txt',
                                        '0_ModCore_UE4SSLuaEventBridge/dlls/main.dll'})
        self.assertEqual(digest, hashlib.sha256(archive).hexdigest())

    def test_rejects_a_wrong_checksum(self):
        product, archive, _, manifest = product_release('UE4SSLuaEventBridge', '1.0.13', 'bridge-events')
        with self.assertRaisesRegex(ValueError, 'checksum'):
            build.verify_product(product, archive, '0' * 64 + '  x.zip\n', manifest, COMMIT)

    def test_rejects_a_manifest_for_another_commit(self):
        product, archive, checksum, manifest = product_release('UE4SSLuaEventBridge', '1.0.13', 'bridge-events')
        with self.assertRaisesRegex(ValueError, 'commit'):
            build.verify_product(product, archive, checksum, manifest, 'b' * 40)

    def test_rejects_a_tools_folder(self):
        product, archive, checksum, manifest = product_release('UE4SSLuaFileBridge', '1.0.1', 'bridge-files', tamper='extra')
        with self.assertRaisesRegex(ValueError, 'tools or tests'):
            build.verify_product(product, archive, checksum, manifest, COMMIT)


class RewriteLinksTests(unittest.TestCase):
    def test_shipped_links_stay_and_others_point_at_the_tagged_source(self):
        shipped = {'README.md', 'CHANGELOG.md', 'docs/DEVELOPERS.md'}
        text = ('[dev](docs/DEVELOPERS.md#limits) [log](CHANGELOG.md) [sample](examples/A/Scripts/main.lua) '
                '[web](https://example.com/x) [here](#top)')
        out = build.rewrite_links(text, 'README.md', shipped, 'o/r', 'bridge-files/v1.0.1', 'bridge-files')
        self.assertIn('(docs/DEVELOPERS.md#limits)', out)
        self.assertIn('(CHANGELOG.md)', out)
        self.assertIn('(https://github.com/o/r/blob/bridge-files/v1.0.1/bridge-files/examples/A/Scripts/main.lua)', out)
        self.assertIn('(https://example.com/x)', out)
        self.assertIn('(#top)', out)

    def test_links_resolve_from_the_document_folder(self):
        out = build.rewrite_links('[d](DEVELOPERS.md) [r](../README.md) [s](../src/a.cpp)', 'docs/BUILD.md',
                                  {'docs/DEVELOPERS.md', 'README.md'}, 'o/r', 't/v1.0.0', 't')
        self.assertIn('(DEVELOPERS.md)', out)
        self.assertIn('(../README.md)', out)
        self.assertIn('(https://github.com/o/r/blob/t/v1.0.0/t/src/a.cpp)', out)


class DocsTests(unittest.TestCase):
    FILES = {
        'bridge-x/README.md': b'# X\n- [Guide](docs/DEVELOPERS.md)\n- [Maintainer guide](docs/BUILD.md): build instructions.\n'
                              b'- [Sample](examples/A/Scripts/main.lua)\n- [Changes](CHANGELOG.md)\n',
        'bridge-x/CHANGELOG.md': b'# Changelog\n',
        'bridge-x/docs/DEVELOPERS.md': b'# Dev\nSee the [maintainer guide](BUILD.md#t).\nKept.\n',
        'bridge-x/docs/BUILD.md': b'# Build\n',
        'bridge-x/docs/SPEC-REVIEW.md': b'# Review\n',
        'bridge-x/docs/API.md': b'# API\n\nStatus: **frozen.** Process note\nacross two lines.\n\nIntro stays.\n\n'
                                b'- [Used](#used)\n- [Internal native contract](#internal-native-contract)\n\n'
                                b'## Used\n\nUsage.\n\n## Internal native contract\n\nSecret.\n\n## Open points for review\n\n- p\n',
        'bridge-x/examples/A/Scripts/main.lua': b'print(1)\n',
        'bridge-x/examples/A/enabled.txt': b'1',
    }

    def docs(self):
        product = {'folder_in_repo': 'bridge-x', 'product_name': 'X', 'tag': 'bridge-x/v1.0.0'}

        def list_docs(path):
            return [name[len('bridge-x/'):] for name in self.FILES if name.startswith(path + '/')]

        return build.product_docs(product, self.FILES.get, list_docs)

    def test_only_usage_documentation_and_examples_ship(self):
        self.assertEqual(sorted(self.docs()), ['docs/X/README.md', 'docs/X/docs/API.md', 'docs/X/docs/DEVELOPERS.md',
                                               'docs/X/examples/A/Scripts/main.lua'])

    def test_development_sections_and_status_paragraph_are_dropped(self):
        api = self.docs()['docs/X/docs/API.md'].decode()
        self.assertIn('Intro stays.', api)
        self.assertIn('Usage.', api)
        for dropped in ('Status:', 'Process note', 'Secret', 'Internal native', 'Open points', 'across two lines'):
            self.assertNotIn(dropped, api)

    def test_no_shipped_text_mentions_build_or_review_documents(self):
        for name, data in self.docs().items():
            for dropped in ('BUILD.md', 'SPEC-REVIEW.md', 'Maintainer'):
                self.assertNotIn(dropped, data.decode(), name)
        self.assertIn('Kept.', self.docs()['docs/X/docs/DEVELOPERS.md'].decode())


class AssembleTests(unittest.TestCase):
    def setUp(self):
        self.events = verified('UE4SSLuaEventBridge', '1.0.13', 'bridge-events',
                               {'docs/UE4SSLuaEventBridge/README.md': b'events'})
        self.files = verified('UE4SSLuaFileBridge', '1.0.1', 'bridge-files',
                              {'docs/UE4SSLuaFileBridge/README.md': b'files'})
        self.readme = ('# Title\n[E](bridge-events/README.md) [D](bridge-files/docs/API.md#x) '
                       '[G](DEVELOPMENT.md)\n')

    def build(self, directory, version='1.1.0', readme=None):
        return build.assemble(version, 'c' * 40, [self.events, self.files], readme or self.readme,
                              'MIT\n', directory, STAMP)

    def test_package_holds_both_folders_and_shared_documentation_beside_them(self):
        with tempfile.TemporaryDirectory() as directory:
            target = self.build(directory)
            self.assertEqual(target.name, 'UE4SSLuaEXtensions-v1.1.0.zip')
            with zipfile.ZipFile(target) as z:
                names = z.namelist()
                shared = '0_ModCore_UE4SSLuaEXtensions/'
                readme = z.read(shared + 'README.md').decode()
                self.assertIn('[E](docs/UE4SSLuaEventBridge/README.md)', readme)
                self.assertIn('(https://github.com/jrpereira/UE4SSLuaEXtensions/blob/' + 'c' * 40 + '/DEVELOPMENT.md)', readme)
                self.assertEqual(z.read(shared + 'docs/UE4SSLuaFileBridge/README.md'), b'files')
            folders = {name.split('/')[0] for name in names}
            self.assertEqual(folders, {'0_ModCore_UE4SSLuaEXtensions', '0_ModCore_UE4SSLuaEventBridge',
                                       '0_ModCore_UE4SSLuaFileBridge'})
            self.assertEqual([n for n in names if '/' not in n], [], 'no files at the ZIP root')
            for folder in ('0_ModCore_UE4SSLuaEventBridge/', '0_ModCore_UE4SSLuaFileBridge/'):
                self.assertFalse([n for n in names if n.startswith(folder) and n.lower().endswith(('.md', '.bb'))])

    def test_mod_folder_bytes_are_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            target = self.build(directory)
            with zipfile.ZipFile(target) as z:
                for product in (self.events, self.files):
                    for name, (_, data) in product['entries'].items():
                        self.assertEqual(z.read(name), data)

    def test_checksum_and_manifest_describe_the_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            target = self.build(directory)
            digest = hashlib.sha256(target.read_bytes()).hexdigest()
            self.assertEqual(Path(f'{target}.sha256').read_text().split()[0], digest)
            manifest = json.loads(Path(f'{target}.manifest.json').read_text())
            self.assertEqual([p['tag'] for p in manifest['products']], ['bridge-events/v1.0.13', 'bridge-files/v1.0.1'])
            self.assertEqual(manifest['version'], '1.1.0')
            with zipfile.ZipFile(target) as z:
                self.assertEqual({f['path'] for f in manifest['files']}, set(z.namelist()))
                self.assertTrue(all(n.startswith('0_') for n in z.namelist()))

    def test_output_is_deterministic(self):
        with tempfile.TemporaryDirectory() as one, tempfile.TemporaryDirectory() as two:
            self.assertEqual(self.build(one).read_bytes(), self.build(two).read_bytes())

    def test_refuses_to_overwrite_and_bad_versions(self):
        with tempfile.TemporaryDirectory() as directory:
            self.build(directory)
            with self.assertRaises(FileExistsError):
                self.build(directory)
            with self.assertRaises(ValueError):
                self.build(directory, version='1.1')

    def test_no_tools_or_tests_folder_anywhere_in_the_package(self):
        with tempfile.TemporaryDirectory() as directory:
            target = self.build(directory)
            with zipfile.ZipFile(target) as z:
                for name in z.namelist():
                    self.assertFalse({p.lower() for p in name.split('/')[:-1]} & {'tools', 'tests'}, name)
        self.events['docs']['docs/UE4SSLuaEventBridge/tests/notes.md'] = b'x'
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, 'tools or tests'):
                self.build(directory)


if __name__ == '__main__':
    unittest.main()
