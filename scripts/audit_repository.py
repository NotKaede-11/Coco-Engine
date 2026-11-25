"""Audit tracked and publishable untracked files without changing the Git index.

Checks names, sizes, personal paths and a small set of high-confidence secret
patterns. This is a working-tree review, not a history or complete secret audit.
Reports only paths/line numbers, never matched values. Ignored data stays local.
"""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import subprocess

from check_public_paths import violations

ROOT = Path(__file__).resolve().parents[1]
SECRET = re.compile(
    rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"
    rb"|gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}"
    rb"|AKIA[A-Z0-9]{16}"
)
PAYLOADS = {'.zip', '.7z', '.ckpt', '.pt', '.pth', '.binpack', '.safetensors', '.exe', '.dll', '.o', '.obj'}


def git_files(*args):
    # Review repository rules independently of the user's global excludes.
    result = subprocess.run(
        ['git', '-c', 'core.excludesFile=' + os.devnull, 'ls-files', '-z', *args],
        cwd=ROOT, capture_output=True, check=True,
    )
    return [name for name in result.stdout.decode('utf-8').split('\0') if name]


def audit():
    config = json.loads((ROOT / 'release.json').read_text())
    selected_network = config.get('nnue_file', 'coco.nnue')
    tracked = git_files('--cached')
    untracked = git_files('--others', '--exclude-standard')
    findings, skipped, checked = [], [], 0
    total_bytes = 0
    for name in sorted(set(tracked + untracked)):
        path = ROOT / name
        if path.is_symlink() or not path.is_file():
            skipped.append({'path': name, 'reason': 'symlink or unavailable file'})
            continue
        size = path.stat().st_size
        total_bytes += size
        if path.suffix.lower() in PAYLOADS:
            findings.append({'path': name, 'kind': 'publishable generated payload'})
        if size > 10 * 1024 * 1024 and name != selected_network:
            findings.append({'path': name, 'kind': 'file exceeds 10 MiB review threshold'})
        if size > 2 * 1024 * 1024:
            skipped.append({'path': name, 'reason': 'content exceeds 2 MiB scan budget'})
            continue
        data = path.read_bytes()
        if b'\0' in data and path.suffix.lower() not in {'.md', '.txt', '.json'}:
            skipped.append({'path': name, 'reason': 'binary content; metadata only'})
            continue
        checked += 1
        lines = violations(data)
        if lines:
            findings.append({'path': name, 'kind': 'personal/local path', 'lines': lines})
        secret_lines = sorted({data.count(b'\n', 0, match.start()) + 1 for match in SECRET.finditer(data)})
        if secret_lines:
            findings.append({'path': name, 'kind': 'possible credential', 'lines': secret_lines})
    return {
        'scope': 'current tracked files plus nonignored untracked candidates; not Git history',
        'tracked_files': len(tracked), 'untracked_candidates': len(untracked),
        'candidate_bytes': total_bytes, 'content_checked_files': checked,
        'untracked_by_directory': dict(Counter(name.split('/')[0] for name in untracked)),
        'findings': findings, 'content_limits': skipped,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    report = audit()
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"Reviewed {report['tracked_files']} tracked files and {report['untracked_candidates']} "
          f"untracked candidates; {len(report['findings'])} findings; "
          f"{len(report['content_limits'])} content limitations.")
    for issue in report['findings']:
        print(f"{issue['kind']}: {issue['path']} (lines {issue.get('lines', [])})")
    return bool(report['findings'])


if __name__ == '__main__':
    raise SystemExit(main())
