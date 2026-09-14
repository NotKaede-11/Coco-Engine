"""Reject personal home-directory paths and local file links in publishable files.

This is a path-hygiene check, not a credential or archive-content scanner.
Known hosted-runner accounts and documentation placeholders are allowed.
"""
import argparse
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
PATTERN = re.compile(r"""(?i)(?:[a-z]:[\\/]+(?:Users|Documents and Settings)[\\/]+([^\\/\s"'<>]+)|/(?:Users|home)/([^/\s"'<>]+)|""" + 'file:' + r"""//[^\s"'<>]+)""")
ALLOWED = {'public','default','runner','runneradmin','username','user','yourname','example'}


def violations(data):
    text = unquote(data.replace(b'\x00', b'').decode('utf-8', errors='replace'))
    lines = set()
    for number, line in enumerate(text.splitlines(), 1):
        for match in PATTERN.finditer(line):
            account = match.group(1) or match.group(2)
            if account is None or account.lower() not in ALLOWED:
                lines.add(number)
    return sorted(lines)


def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision', help='Scan a Git snapshot instead of tracked working files')
    args = parser.parse_args()
    files = []
    if args.revision:
        for entry in git('ls-tree', '-rz', args.revision).split(b'\0'):
            if entry:
                meta, name = entry.split(b'\t', 1)
                mode, kind, oid = meta.decode().split()
                if kind == 'blob':
                    files.append((name.decode(), git('cat-file','blob',oid)))
    else:
        files = [(name, (ROOT/name).read_bytes()) for name in git('ls-files','-z').decode().split('\0') if name and (ROOT/name).is_file()]
    found = [(name, lines) for name,data in files if (lines := violations(data))]
    for name,lines in found:
        print(f'Personal/local path in {name}: lines {", ".join(map(str,lines))}')
    if found:
        raise SystemExit(1)
    print(f'PASS: {len(files)} tracked files; no detected personal home paths or file links')


if __name__ == '__main__':
    main()
