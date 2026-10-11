"""Assemble the combined Nexus package from two published product releases.

The package holds the mod folders of UE4SSLuaEventBridge and UE4SSLuaFileBridge,
copied byte for byte from their release ZIPs after each ZIP, checksum and payload
manifest has been verified, plus the shared documentation in a folder of its own beside
them: a top-level README, the LICENSE, and each extension's documentation under
docs/<ProductName>/. The ZIP has no files at its root, only folders starting with 0_.
Nothing is built here. It runs in CI so the result has a public hash record.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import posixpath
import re
import subprocess
import sys
import tempfile
import time
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from release_notes import extract_notes

PACKAGE = 'UE4SSLuaEXtensions'
REPOSITORY = 'jrpereira/UE4SSLuaEXtensions'
SHARED_FOLDER = '0_ModCore_UE4SSLuaEXtensions'
VERSION = re.compile(r'[0-9]+\.[0-9]+\.[0-9]+')
# Documentation shipped for each extension, relative to its folder in the repository.
DOC_FILES = ('README.md', 'CHANGELOG.md')
DOC_FOLDER = 'docs'
DOC_EXCLUDED = ('SPEC-REVIEW.md',)
# Development folders never ship in a package, at any depth.
EXCLUDED_FOLDERS = {'tools', 'tests'}
LINK = re.compile(r'(\]\()([^)\s]+)(\))')


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def verify_product(product, archive, checksum, manifest, commit):
    """Check one product release; return its mod folder entries as {name: (info, bytes)}."""
    name = product['product_name']
    version = product['version']
    expected = f'{name}-v{version}.zip'
    digest = sha256(archive)
    if checksum.split()[0].lower() != digest:
        raise ValueError(f'{expected}: checksum does not match the published .sha256')
    described = json.loads(manifest)
    if described.get('product') != name or described.get('version') != version:
        raise ValueError(f'{expected}: payload manifest names {described.get("product")} {described.get("version")}')
    if described.get('source_commit') != commit:
        raise ValueError(f'{expected}: payload manifest commit {described.get("source_commit")} is not the tag commit {commit}')
    folder = f'0_ModCore_{name}/'
    entries = {}
    with zipfile.ZipFile(io.BytesIO(archive)) as z:
        if z.testzip() is not None:
            raise ValueError(f'{expected}: corrupt archive')
        for info in z.infolist():
            if info.is_dir():
                continue
            if not info.filename.startswith(folder) or '..' in info.filename.split('/'):
                raise ValueError(f'{expected}: unexpected entry {info.filename}')
            if set(part.lower() for part in info.filename.split('/')[:-1]) & EXCLUDED_FOLDERS:
                raise ValueError(f'{expected}: contains a tools or tests folder')
            entries[info.filename] = (info.date_time, z.read(info))
    files = {item['path']: item['sha256'] for item in described['files']}
    if set(files) != set(entries):
        raise ValueError(f'{expected}: archive entries differ from the payload manifest')
    for path, (_, data) in entries.items():
        if sha256(data) != files[path]:
            raise ValueError(f'{expected}: {path} does not match the payload manifest')
    return entries, digest


def rewrite_links(text, source_path, shipped, repository, tag, product_folder):
    """Point relative links at shipped documents, or at the tagged source when not shipped."""
    base = posixpath.dirname(source_path)

    def replace(match):
        target = match.group(2)
        if re.match(r'^([a-z][a-z0-9+.-]*:|#|/)', target, re.I):
            return match.group(0)
        path, _, anchor = target.partition('#')
        resolved = posixpath.normpath(posixpath.join(base, path))
        if resolved in shipped:
            return match.group(0)
        url = f'https://github.com/{repository}/blob/{tag}/{product_folder}/{resolved}'
        return f'{match.group(1)}{url}{"#" + anchor if anchor else ""}{match.group(3)}'

    return LINK.sub(replace, text)


def product_docs(product, read_file, list_docs):
    """The documents shipped for one extension, as {path in the ZIP: bytes}."""
    folder = product['folder_in_repo']
    wanted = [path for path in DOC_FILES if read_file(f'{folder}/{path}') is not None]
    wanted += [path for path in list_docs(f'{folder}/{DOC_FOLDER}')
               if path.endswith('.md') and posixpath.basename(path) not in DOC_EXCLUDED]
    wanted = sorted(set(wanted))
    shipped = set(wanted)
    docs = {}
    for path in wanted:
        text = read_file(f'{folder}/{path}').decode('utf-8')
        text = rewrite_links(text, path, shipped, REPOSITORY, product['tag'], folder)
        docs[f'docs/{product["product_name"]}/{path}'] = text.encode('utf-8')
    return docs


def render_readme(template, collection_version, versions):
    text = template.replace('{{COLLECTION_VERSION}}', collection_version)
    text = text.replace('{{EVENT_BRIDGE_VERSION}}', versions['UE4SSLuaEventBridge'])
    text = text.replace('{{FILE_BRIDGE_VERSION}}', versions['UE4SSLuaFileBridge'])
    if '{{' in text:
        raise ValueError('The collection README has an unfilled placeholder')
    return text


def assemble(collection_version, source_commit, products, readme, license_text, output, date_time):
    """Write the package ZIP, its .sha256 and .manifest.json; return the ZIP's path."""
    if not VERSION.fullmatch(collection_version):
        raise ValueError('The collection version must look like 1.1.0')
    archive_name = f'{PACKAGE}-v{collection_version}.zip'
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    target = output / archive_name
    for path in (target, Path(f'{target}.sha256'), Path(f'{target}.manifest.json')):
        if path.exists():
            raise FileExistsError(f'Refusing to overwrite {path}')
    versions = {product['product_name']: product['version'] for product in products}
    members = {f'{SHARED_FOLDER}/README.md': render_readme(readme, collection_version, versions).encode('utf-8'),
               f'{SHARED_FOLDER}/LICENSE': license_text.encode('utf-8')}
    dated = {name: date_time for name in members}
    for product in products:
        for name, data in product['docs'].items():
            members[f'{SHARED_FOLDER}/{name}'] = data
            dated[f'{SHARED_FOLDER}/{name}'] = date_time
    for product in products:
        for name, (stamp, data) in product['entries'].items():
            if name in members:
                raise ValueError(f'Duplicate package entry: {name}')
            members[name] = data
            dated[name] = stamp
    for name in members:
        if not name.startswith('0_') or '/' not in name:
            raise ValueError(f'Every package entry must sit in a folder starting with 0_: {name}')
        if set(part.lower() for part in name.split('/')[:-1]) & EXCLUDED_FOLDERS:
            raise ValueError(f'The package must not contain tools or tests: {name}')
    ordered = sorted(members, key=lambda name: (not name.startswith(SHARED_FOLDER + '/'), name))
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in ordered:
            info = zipfile.ZipInfo(name, dated[name])
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, members[name])
    digest = sha256(target.read_bytes())
    Path(f'{target}.sha256').write_text(f'{digest}  {archive_name}\n', encoding='utf-8')
    manifest = {
        'schema': 1,
        'package': PACKAGE,
        'version': collection_version,
        'source_commit': source_commit,
        'products': [{'product': product['product_name'], 'version': product['version'], 'tag': product['tag'],
                      'source_commit': product['commit'], 'archive': product['archive'],
                      'archive_sha256': product['archive_sha256']} for product in products],
        'files': [{'path': name, 'sha256': sha256(members[name])} for name in sorted(members)],
    }
    Path(f'{target}.manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return target


def release_notes(collection_version, products, changelogs):
    lines = ['### Changes', '',
             f'UE4SS Lua Extensions {collection_version} packages these extensions together for Nexus. '
             'Each is released separately on GitHub.', '']
    for product in products:
        name, version = product['product_name'], product['version']
        lines += [f'#### {name} {version}', '',
                  f'[GitHub release](https://github.com/{REPOSITORY}/releases/tag/{product["tag"]})', '',
                  extract_notes(changelogs[product['product_name']], f'v{version}').strip(), '']
    return '\n'.join(lines).rstrip() + '\n'


def git(*args, binary=False):
    result = subprocess.run(['git', *args], capture_output=True, check=False)
    if result.returncode:
        return None
    return result.stdout if binary else result.stdout.decode('utf-8').strip()


def gh_download(tag, pattern, directory):
    subprocess.run(['gh', 'release', 'download', tag, '--repo', REPOSITORY, '--pattern', pattern,
                    '--dir', str(directory), '--clobber'], check=True)


def load_product(tag, folder_in_repo, product_name, work):
    """Download and verify one product release, and read its docs from the tag."""
    match = re.fullmatch(rf'{re.escape(folder_in_repo)}/v({VERSION.pattern})', tag)
    if not match:
        raise ValueError(f'Expected a tag like {folder_in_repo}/v1.0.0, got {tag}')
    version = match.group(1)
    commit = git('rev-parse', f'refs/tags/{tag}^{{commit}}')
    if not commit:
        raise ValueError(f'Tag {tag} is not in this checkout; fetch tags first')
    archive_name = f'{product_name}-v{version}.zip'
    directory = Path(work) / folder_in_repo
    directory.mkdir(parents=True, exist_ok=True)
    gh_download(tag, f'{archive_name}*', directory)
    product = {'product_name': product_name, 'version': version, 'tag': tag, 'commit': commit,
               'archive': archive_name, 'folder_in_repo': folder_in_repo}
    entries, digest = verify_product(
        product, (directory / archive_name).read_bytes(),
        (directory / f'{archive_name}.sha256').read_text(encoding='utf-8'),
        (directory / f'{archive_name}.manifest.json').read_bytes(), commit)

    def read_file(path):
        return git('show', f'{tag}:{path}', binary=True)

    def list_docs(path):
        listing = git('ls-tree', '-r', '--name-only', tag, f'{path}/')
        prefix = f'{folder_in_repo}/'
        return [line[len(prefix):] for line in (listing or '').splitlines()]

    product.update(entries=entries, archive_sha256=digest, docs=product_docs(product, read_file, list_docs),
                   changelog=read_file(f'{folder_in_repo}/CHANGELOG.md').decode('utf-8'))
    return product


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    parser.add_argument('--events-tag', required=True)
    parser.add_argument('--files-tag', required=True)
    parser.add_argument('--output', type=Path, default=Path('collection-dist'))
    parser.add_argument('--output-file', type=Path, help='append key=value lines for GitHub Actions')
    args = parser.parse_args()
    source_commit = git('rev-parse', 'HEAD')
    stamp = int(git('log', '-1', '--format=%ct', 'HEAD'))
    date_time = time.gmtime(stamp)[:6]
    with tempfile.TemporaryDirectory() as work:
        products = [load_product(args.events_tag, 'bridge-events', 'UE4SSLuaEventBridge', work),
                    load_product(args.files_tag, 'bridge-files', 'UE4SSLuaFileBridge', work)]
        root = Path(__file__).resolve().parent.parent
        target = assemble(args.version, source_commit, products,
                          (root / 'collection/README.md').read_text(encoding='utf-8'),
                          (root / 'LICENSE').read_text(encoding='utf-8'), args.output, date_time)
        notes = release_notes(args.version, products, {p['product_name']: p['changelog'] for p in products})
        (args.output / 'release-notes.md').write_text(notes, encoding='utf-8')
    if args.output_file:
        with args.output_file.open('a', encoding='utf-8') as handle:
            handle.write(f'archive={target}\ntag=extensions/v{args.version}\n')
    print(f'{target} {sha256(target.read_bytes())}')


if __name__ == '__main__':
    main()
