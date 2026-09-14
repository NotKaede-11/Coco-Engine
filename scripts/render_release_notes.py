#!/usr/bin/env python3
"""Render reviewed Markdown for GitHub Releases, resolving repository file links."""
import argparse
from pathlib import Path
import re
from urllib.parse import quote

from check_release import ROOT, validate


def render(repository: str, tag: str) -> str:
    config = validate(tag=tag)
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('Invalid GitHub repository')
    source = ROOT / config['notes']
    def replace(match):
        label, target = match.groups()
        if re.match(r'[a-zA-Z][a-zA-Z0-9+.-]*:', target) or target.startswith('#'):
            return match.group(0)
        path, separator, fragment = target.partition('#')
        resolved = (source.parent / path).resolve()
        if not resolved.is_relative_to(ROOT.resolve()) or not resolved.is_file():
            raise ValueError(f'Invalid release note link: {target}')
        relative = resolved.relative_to(ROOT.resolve()).as_posix()
        url = f'https://github.com/{repository}/blob/{quote(tag, safe="")}/{quote(relative)}'
        return f'[{label}]({url}{separator}{fragment})'
    return re.sub(r'\[([^\]]+)\]\(([^)]+)\)', replace, source.read_text(encoding='utf-8'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', required=True)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(render(args.repository, args.tag), encoding='utf-8', newline='\n')
    print(f'Wrote release notes: {args.output}')


if __name__ == '__main__':
    main()
