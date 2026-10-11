"""Assemble the combined Nexus package from published extension releases.

What goes into the package is declared in package-manifest.json. Each extension's mod
folder is copied byte for byte from its release ZIP after the ZIP, its checksum and its
payload manifest have been verified, and the documents the manifest lists are added to
that same folder under docs/. The ZIP holds only folders starting with 0_; the root
README is not deployed, it is the source of the Nexus description (NEXUS.bb).
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

ROOT = Path(__file__).resolve().parent.parent
VERSION = re.compile(r'[0-9]+\.[0-9]+\.[0-9]+')
LINK = re.compile(r'(\]\()([^)\s]+)(\))')


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def load_manifest(path=ROOT / 'package-manifest.json'):
    manifest = json.loads(Path(path).read_text(encoding='utf-8'))
    if manifest.get('schema') != 1:
        raise ValueError('Unsupported package manifest schema')
    for key in ('package', 'repository', 'archive', 'release_tag', 'forbidden_folders', 'filters', 'extensions'):
        if key not in manifest:
            raise ValueError(f'The package manifest lacks "{key}"')
    for extension in manifest['extensions'] + manifest.get('folders', []):
        if extension['mod_folder'] != f'0_ModCore_{extension["name"]}':
            raise ValueError(f'{extension["name"]}: the mod folder must be 0_ModCore_<name>')
    return manifest


def forbidden(path, manifest):
    return bool({part.lower() for part in path.split('/')[:-1]} & {f.lower() for f in manifest['forbidden_folders']})


def verify_product(product, archive, checksum, manifest_bytes, commit, package):
    """Check one extension release; return its mod folder entries as {name: (date_time, bytes)}."""
    name, version = product['product_name'], product['version']
    expected = f'{name}-v{version}.zip'
    digest = sha256(archive)
    if checksum.split()[0].lower() != digest:
        raise ValueError(f'{expected}: checksum does not match the published .sha256')
    described = json.loads(manifest_bytes)
    if described.get('product') != name or described.get('version') != version:
        raise ValueError(f'{expected}: payload manifest names {described.get("product")} {described.get("version")}')
    if described.get('source_commit') != commit:
        raise ValueError(f'{expected}: payload manifest commit {described.get("source_commit")} is not the tag commit {commit}')
    folder = product['mod_folder'] + '/'
    entries = {}
    with zipfile.ZipFile(io.BytesIO(archive)) as z:
        if z.testzip() is not None:
            raise ValueError(f'{expected}: corrupt archive')
        for info in z.infolist():
            if info.is_dir():
                continue
            if not info.filename.startswith(folder) or '..' in info.filename.split('/'):
                raise ValueError(f'{expected}: unexpected entry {info.filename}')
            if forbidden(info.filename, package):
                raise ValueError(f'{expected}: contains a tools or tests folder')
            entries[info.filename] = (info.date_time, z.read(info))
    files = {item['path']: item['sha256'] for item in described['files']}
    if set(files) != set(entries):
        raise ValueError(f'{expected}: archive entries differ from the payload manifest')
    for path, (_, data) in entries.items():
        if sha256(data) != files[path]:
            raise ValueError(f'{expected}: {path} does not match the payload manifest')
    return entries, digest


def glob_regex(pattern):
    out = ''
    index = 0
    while index < len(pattern):
        if pattern.startswith('**/', index):
            out += '(?:.*/)?'
            index += 3
        elif pattern[index] == '*':
            out += '[^/]*'
            index += 1
        else:
            out += re.escape(pattern[index])
            index += 1
    return re.compile(out)


def plan_documents(extension, files):
    """Resolve an extension's document rules to {destination in the mod folder: source}.

    A source is relative to the extension's folder in the repository, or starts with / for
    a path from the repository root. Every rule must match something."""
    plan = {}
    for rule in extension['documents']:
        source = rule['from']
        if source.startswith('/'):
            matches = [source] if source[1:] in files['root'] else []
            names = {source: rule['to']}
        else:
            regex = glob_regex(source)
            literal = source.split('*')[0]
            prefix = literal[:literal.rfind('/') + 1]
            matches = sorted(path for path in files['extension'] if regex.fullmatch(path)
                             and posixpath.basename(path) not in rule.get('except', []))
            names = {path: rule['to'] + path[len(prefix):] if rule['to'].endswith('/') else rule['to']
                     for path in matches}
        if not matches:
            raise ValueError(f'{extension["name"]}: the document rule for {source} matches nothing')
        for path in matches:
            destination = names[path]
            if destination in plan and plan[destination] != path:
                raise ValueError(f'{extension["name"]}: {destination} comes from two sources')
            plan[destination] = path
    return plan


def drop_development_text(text, filters):
    """Remove what concerns the extension's development from a shipped document."""
    sections = set(filters['drop_sections'])
    prefixes = tuple(filters['drop_paragraph_prefixes'])
    references = filters['drop_lines_containing']
    kept = []
    skipping = None  # 'section' until the next ## heading, 'paragraph' until a blank line
    for line in text.splitlines(keepends=True):
        heading = re.match(r'^## (.+?)\s*$', line)
        if skipping == 'section' and not heading:
            continue
        if skipping == 'paragraph':
            if line.strip():
                continue
            skipping = None
        if heading:
            skipping = 'section' if heading.group(1) in sections else None
            if skipping:
                continue
        if prefixes and line.startswith(prefixes):
            skipping = 'paragraph'
            continue
        if any(name in line for name in references):
            continue
        kept.append(line)
    return ''.join(kept)


def rewrite_links(text, source, plan_by_source, repository, tag, folder):
    """Point relative links at the shipped copy of a document, or at the tagged source."""
    base = posixpath.dirname(source)
    here = posixpath.dirname(plan_by_source[source])

    def replace(match):
        target = match.group(2)
        if re.match(r'^([a-z][a-z0-9+.-]*:|#|/)', target, re.I):
            return match.group(0)
        path, _, anchor = target.partition('#')
        resolved = posixpath.normpath(posixpath.join(base, path))
        suffix = f'#{anchor}' if anchor else ''
        if resolved in plan_by_source:
            new = posixpath.relpath(plan_by_source[resolved], here or '.')
        else:
            new = f'https://github.com/{repository}/blob/{tag}/{posixpath.join(folder, resolved)}'
        return f'{match.group(1)}{new}{suffix}{match.group(3)}'

    return LINK.sub(replace, text)


def document_plan(extension, list_files):
    """{destination in the mod folder: source} for one extension at a tag."""
    folder = extension['folder']
    files = {'extension': [path[len(folder) + 1:] for path in list_files(folder)],
             'root': set(list_files(''))}
    return plan_documents(extension, files)


def product_documents(extension, tag, manifest, read_file, list_files):
    """The documents shipped for one extension, as {path inside its mod folder: bytes}."""
    folder = extension['folder']
    plan = document_plan(extension, list_files)
    by_source = {source: destination for destination, source in plan.items()}
    documents = {}
    for destination, source in sorted(plan.items()):
        data = read_file(source[1:] if source.startswith('/') else f'{folder}/{source}')
        if data is None:
            raise ValueError(f'{extension["name"]}: {source} is missing at {tag}')
        if destination.endswith('.md'):
            text = drop_development_text(data.decode('utf-8'), manifest['filters'])
            data = rewrite_links(text, source, by_source, manifest['repository'], tag, folder).encode('utf-8')
        if forbidden(f'{extension["mod_folder"]}/{destination}', manifest):
            raise ValueError(f'{extension["name"]}: {destination} is inside a tools or tests folder')
        documents[destination] = data
    return documents


def link_targets(extensions, plans):
    """Where each extension document lands in the ZIP, by its repository path."""
    targets = {}
    for extension, plan in zip(extensions, plans):
        for destination, source in plan.items():
            if not source.startswith('/'):
                targets[f'{extension["folder"]}/{source}'] = f'{extension["mod_folder"]}/{destination}'
    return targets


def package_folders(manifest, collection_version, read_file, targets=None, commit=''):
    """The package's own mod folders (no DLL, versioned with the package), as
    [{'name', 'mod_folder', 'source', 'files': {path in the folder: bytes}}].

    A folder's documents come from the repository root (a rule's source starts with /); their
    relative links point at the documents' places in the ZIP, or at the source at commit."""
    folders = []
    for folder in manifest.get('folders', []):
        files = {}
        for rule in folder.get('documents', []):
            source = rule['from'].lstrip('/')
            data = read_file(source)
            if data is None:
                raise ValueError(f'{folder["name"]}: {source} is missing')
            here = f'{folder["mod_folder"]}/{rule["to"]}'
            text = drop_development_text(data.decode('utf-8'), manifest['filters'])

            def replace(match, source=source, here=here):
                target = match.group(2)
                if re.match(r'^([a-z][a-z0-9+.-]*:|#|/)', target, re.I):
                    return match.group(0)
                path, _, anchor = target.partition('#')
                resolved = posixpath.normpath(posixpath.join(posixpath.dirname(source), path))
                if resolved in (targets or {}):
                    new = posixpath.relpath(targets[resolved], posixpath.dirname(here))
                else:
                    new = f'https://github.com/{manifest["repository"]}/blob/{commit}/{resolved}'
                return f'{match.group(1)}{new}{"#" + anchor if anchor else ""}{match.group(3)}'

            files[rule['to']] = LINK.sub(replace, text).encode('utf-8')
        for path in folder['files']:
            data = read_file(f'{folder["source"]}/{path}')
            if data is None:
                raise ValueError(f'{folder["name"]}: {folder["source"]}/{path} is missing')
            if path == 'mod.json':
                described = json.loads(data.decode('utf-8'))
                packaged = {}
                for key, value in described.items():
                    if key in ('version', 'version_source'):
                        continue
                    packaged[key] = value
                    if key == 'name':
                        packaged['version'] = collection_version
                data = (json.dumps(packaged, indent=2) + '\n').encode('utf-8')
            files[path] = data
        folders.append({'name': folder['name'], 'mod_folder': folder['mod_folder'],
                        'source': folder['source'], 'files': files})
    return folders


def assemble(manifest, collection_version, source_commit, products, output, date_time, folders=()):
    """Write the package ZIP, its .sha256 and .manifest.json; return the ZIP's path."""
    if not VERSION.fullmatch(collection_version):
        raise ValueError('The collection version must look like 1.1.0')
    archive_name = manifest['archive'].format(version=collection_version)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    target = output / archive_name
    for path in (target, Path(f'{target}.sha256'), Path(f'{target}.manifest.json')):
        if path.exists():
            raise FileExistsError(f'Refusing to overwrite {path}')
    members, dated = {}, {}
    for product in products:
        for name, (stamp, data) in product['entries'].items():
            members[name] = data
            dated[name] = stamp
        for destination, data in product['docs'].items():
            name = f'{product["mod_folder"]}/{destination}'
            if name in members:
                raise ValueError(f'Duplicate package entry: {name}')
            members[name] = data
            dated[name] = date_time
    for folder in folders:
        for path, data in folder['files'].items():
            name = f'{folder["mod_folder"]}/{path}'
            if name in members:
                raise ValueError(f'Duplicate package entry: {name}')
            members[name] = data
            dated[name] = date_time
    for name in members:
        if not name.startswith('0_') or '/' not in name:
            raise ValueError(f'Every package entry must sit in a folder starting with 0_: {name}')
        if forbidden(name, manifest):
            raise ValueError(f'The package must not contain tools or tests: {name}')
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in sorted(members):
            info = zipfile.ZipInfo(name, dated[name])
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, members[name])
    digest = sha256(target.read_bytes())
    Path(f'{target}.sha256').write_text(f'{digest}  {archive_name}\n', encoding='utf-8')
    described = {
        'schema': 1,
        'package': manifest['package'],
        'version': collection_version,
        'source_commit': source_commit,
        'products': [{'product': product['product_name'], 'version': product['version'], 'tag': product['tag'],
                      'source_commit': product['commit'], 'archive': product['archive'],
                      'archive_sha256': product['archive_sha256']} for product in products],
        'folders': [{'folder': folder['mod_folder'], 'source': folder['source']} for folder in folders],
        'files': [{'path': name, 'sha256': sha256(members[name])} for name in sorted(members)],
    }
    Path(f'{target}.manifest.json').write_text(json.dumps(described, indent=2) + '\n', encoding='utf-8')
    return target


def release_notes(manifest, collection_version, products, changelogs):
    lines = ['### Changes', '',
             f'{manifest["package"]} {collection_version} packages these extensions together for Nexus. '
             'Each is released separately on GitHub.', '']
    for product in products:
        name, version = product['product_name'], product['version']
        lines += [f'#### {name} {version}', '',
                  f'[GitHub release](https://github.com/{manifest["repository"]}/releases/tag/{product["tag"]})', '',
                  extract_notes(changelogs[name], f'v{version}').strip(), '']
    return '\n'.join(lines).rstrip() + '\n'


def git(*args, binary=False):
    result = subprocess.run(['git', *args], capture_output=True, check=False)
    if result.returncode:
        return None
    return result.stdout if binary else result.stdout.decode('utf-8').strip()


def load_product(manifest, extension, tag, work):
    """Download and verify one extension release, and read its documents from the tag."""
    folder, name = extension['folder'], extension['name']
    match = re.fullmatch(rf'{re.escape(folder)}/v({VERSION.pattern})', tag)
    if not match:
        raise ValueError(f'Expected a tag like {folder}/v1.0.0, got {tag}')
    version = match.group(1)
    commit = git('rev-parse', f'refs/tags/{tag}^{{commit}}')
    if not commit:
        raise ValueError(f'Tag {tag} is not in this checkout; fetch tags first')
    archive_name = f'{name}-v{version}.zip'
    directory = Path(work) / folder
    directory.mkdir(parents=True, exist_ok=True)
    subprocess.run(['gh', 'release', 'download', tag, '--repo', manifest['repository'],
                    '--pattern', f'{archive_name}*', '--dir', str(directory), '--clobber'], check=True)
    product = {'product_name': name, 'version': version, 'tag': tag, 'commit': commit,
               'archive': archive_name, 'mod_folder': extension['mod_folder']}
    entries, digest = verify_product(
        product, (directory / archive_name).read_bytes(),
        (directory / f'{archive_name}.sha256').read_text(encoding='utf-8'),
        (directory / f'{archive_name}.manifest.json').read_bytes(), commit, manifest)

    def read_file(path):
        return git('show', f'{tag}:{path}', binary=True)

    def list_files(path):
        """Files under a folder of the tag; with no folder, the files at the repository root."""
        listing = git('ls-tree', '-r', '--name-only', tag, f'{path}/') if path else git('ls-tree', '--name-only', tag)
        return (listing or '').splitlines()

    product.update(entries=entries, archive_sha256=digest, plan=document_plan(extension, list_files),
                   docs=product_documents(extension, tag, manifest, read_file, list_files),
                   changelog=read_file(f'{folder}/CHANGELOG.md').decode('utf-8'))
    return product


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    parser.add_argument('--tag', action='append', required=True,
                        help='release tag of one extension, such as bridge-events/v1.0.13; give one per extension')
    parser.add_argument('--output', type=Path, default=Path('collection-dist'))
    parser.add_argument('--output-file', type=Path, help='append key=value lines for GitHub Actions')
    args = parser.parse_args()
    manifest = load_manifest()
    by_folder = {extension['folder']: extension for extension in manifest['extensions']}
    chosen = {}
    for tag in args.tag:
        folder = tag.split('/')[0]
        if folder not in by_folder or folder in chosen:
            parser.error(f'Unexpected or repeated extension tag: {tag}')
        chosen[folder] = tag
    if set(chosen) != set(by_folder):
        parser.error(f'Give one tag for each of: {", ".join(by_folder)}')
    source_commit = git('rev-parse', 'HEAD')
    date_time = time.gmtime(int(git('log', '-1', '--format=%ct', 'HEAD')))[:6]
    with tempfile.TemporaryDirectory() as work:
        products = [load_product(manifest, extension, chosen[extension['folder']], work)
                    for extension in manifest['extensions']]
        targets = link_targets(manifest['extensions'], [product['plan'] for product in products])
        folders = package_folders(manifest, args.version, lambda path: (ROOT / path).read_bytes() if (ROOT / path).is_file() else None,
                                  targets, source_commit)
        target = assemble(manifest, args.version, source_commit, products, args.output, date_time, folders)
        notes = release_notes(manifest, args.version, products, {p['product_name']: p['changelog'] for p in products})
        (args.output / 'release-notes.md').write_text(notes, encoding='utf-8')
    if args.output_file:
        with args.output_file.open('a', encoding='utf-8') as handle:
            handle.write(f'archive={target}\ntag={manifest["release_tag"].format(version=args.version)}\n')
    print(f'{target} {sha256(target.read_bytes())}')


if __name__ == '__main__':
    main()
