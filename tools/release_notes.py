# Vendored from ModCoreSettings (owner). Do not edit copies; change the source and re-vendor.
"""Extract one version's release notes from the consolidated changelog.

Release notes open with a "## Changes" heading (AGENTS.md 7.6), whether or not
the changelog section has one."""
import argparse
from pathlib import Path
import re

HEADING = '## Changes'
LEADING_CHANGES = re.compile(r'#{2,6}[ \t]+Changes[ \t]*(?:(?:\r?\n)+|$)')


def extract_notes(changelog, tag, heading=True):
    """The section for tag, opened with HEADING; heading=False returns the body alone."""
    if not re.fullmatch(r'v[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?', tag):
        raise ValueError('Expected a version tag such as v1.0.1')
    headings = []
    offset = 0
    fence = None
    for line in changelog.splitlines(keepends=True):
        marker = re.match(r'^\s{0,3}(`{3,}|~{3,})', line)
        if marker:
            token = marker[1]
            if fence is None:
                fence = token
            elif token[0] == fence[0] and len(token) >= len(fence) and not line.strip()[len(token):]:
                fence = None
        elif fence is None:
            match = re.match(r'^## ([^\r\n]+)', line)
            if match:
                headings.append((match[1].strip(), offset, offset + len(line)))
        offset += len(line)
    matches = [i for i, found in enumerate(headings) if found[0] == tag]
    if len(matches) != 1:
        raise ValueError(f'Expected exactly one changelog section for {tag}; found {len(matches)}')
    index = matches[0]
    end = headings[index + 1][1] if index + 1 < len(headings) else len(changelog)
    notes = LEADING_CHANGES.sub('', changelog[headings[index][2]:end].strip(), count=1).strip()
    if not notes:
        raise ValueError(f'Empty changelog section for {tag}')
    return (f'{HEADING}\n\n{notes}' if heading else notes) + '\n'


def default_changelog(root=Path('.')):
    """docs/CHANGELOG.md where a module keeps its documents in docs/, else CHANGELOG.md."""
    docs = root / 'docs/CHANGELOG.md'
    return docs if docs.exists() else root / 'CHANGELOG.md'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--changelog', type=Path)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        notes = extract_notes((args.changelog or default_changelog()).read_text(encoding='utf-8'), args.tag)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    args.output.write_text(notes, encoding='utf-8')


if __name__ == '__main__':
    main()
