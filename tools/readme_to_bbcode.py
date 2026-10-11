"""Generate the Nexus description (NEXUS.bb) from the repository README.md.

Only the Markdown the README uses is supported: headings, paragraphs, bullet and
numbered lists, fenced code blocks, and bold, italic, inline code and link spans.
Anything else (tables, images, HTML, block quotes) is an error rather than a
silent mangling. The title heading is dropped: Nexus shows the mod's name itself.
Relative links become links into the repository at the given reference.
"""
import argparse
from pathlib import Path
import re
import sys

REPOSITORY_BLOB = 'https://github.com/jrpereira/UE4SSLuaEXtensions/blob/{ref}/{path}'
SIZES = {2: 4, 3: 3}
FONT = 'Courier New'


def inline(text, ref):
    parts = re.split(r'(`[^`]+`)', text)
    out = []
    for part in parts:
        if part.startswith('`') and part.endswith('`') and len(part) > 2:
            out.append(f'[font={FONT}]{part[1:-1]}[/font]')
            continue
        part = re.sub(r'\[([^\]]+)\]\(([^)\s]+)\)', lambda m: link(m.group(1), m.group(2), ref), part)
        part = re.sub(r'\*\*([^*]+)\*\*', r'[b]\1[/b]', part)
        part = re.sub(r'(?<![*\w])\*([^*\s][^*]*)\*(?![*\w])', r'[i]\1[/i]', part)
        out.append(part)
    return ''.join(out)


def link(label, target, ref):
    if not re.match(r'^[a-z][a-z0-9+.-]*:', target, re.I):
        if target.startswith('#'):
            return label
        target = REPOSITORY_BLOB.format(ref=ref, path=target.lstrip('/'))
    return f'[url={target}]{label}[/url]'


def convert(markdown, ref='main'):
    lines = markdown.splitlines()
    out = []
    paragraph = []
    items = None  # (ordered, [text, ...]) while inside a list
    index = 0

    def flush_paragraph():
        if paragraph:
            out.append(inline(' '.join(paragraph), ref))
            out.append('')
            paragraph.clear()

    def flush_list():
        nonlocal items
        if items:
            ordered, entries = items
            out.append('[list=1]' if ordered else '[list]')
            out.extend(f'[*]{inline(entry, ref)}' for entry in entries)
            out.append('[/list]')
            out.append('')
        items = None

    while index < len(lines):
        line = lines[index]
        index += 1
        fence = re.match(r'^```', line)
        if fence:
            flush_paragraph()
            flush_list()
            block = []
            while index < len(lines) and not lines[index].startswith('```'):
                block.append(lines[index])
                index += 1
            if index >= len(lines):
                raise ValueError('Unclosed code block')
            index += 1
            out.append('[code]' + '\n'.join(block) + '[/code]')
            out.append('')
            continue
        if not line.strip():
            flush_paragraph()
            flush_list()
            continue
        if re.match(r'^\s*(\||>|<|!\[)', line):
            raise ValueError(f'Unsupported Markdown (table, quote, HTML or image): {line!r}')
        heading = re.match(r'^(#{1,6}) (.+?)\s*$', line)
        if heading:
            flush_paragraph()
            flush_list()
            level = len(heading.group(1))
            if level == 1:
                continue
            if level not in SIZES:
                raise ValueError(f'Unsupported heading level: {line!r}')
            out.append(f'[size={SIZES[level]}][b]{inline(heading.group(2), ref)}[/b][/size]')
            continue
        bullet = re.match(r'^[-*] (.+)$', line)
        numbered = re.match(r'^\d+\. (.+)$', line)
        if bullet or numbered:
            flush_paragraph()
            ordered = bool(numbered)
            if items and items[0] != ordered:
                flush_list()
            items = items or (ordered, [])
            items[1].append((bullet or numbered).group(1))
            continue
        if items and line.startswith('  '):
            items[1][-1] += ' ' + line.strip()
            continue
        flush_list()
        paragraph.append(line.strip())
    flush_paragraph()
    flush_list()
    return '\n'.join(out).strip() + '\n'


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--readme', type=Path, default=root / 'README.md')
    parser.add_argument('--output', type=Path, default=root / 'NEXUS.bb')
    parser.add_argument('--ref', default='main', help='Git reference relative links point at')
    parser.add_argument('--check', action='store_true', help='fail if the output file is out of date')
    args = parser.parse_args()
    text = convert(args.readme.read_text(encoding='utf-8'), args.ref)
    if args.check:
        if not args.output.exists() or args.output.read_text(encoding='utf-8') != text:
            print(f'{args.output} is out of date; run tools/readme_to_bbcode.py', file=sys.stderr)
            return 1
        return 0
    args.output.write_text(text, encoding='utf-8', newline='\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
