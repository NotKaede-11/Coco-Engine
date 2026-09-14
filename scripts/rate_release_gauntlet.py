"""Run Ordo on an audited frozen gauntlet; retain commands and sensitivity fits."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshot', type=Path)
    parser.add_argument('--ordo', type=Path, default=Path('tools/ordo/ordo.exe'))
    args = parser.parse_args()
    root = args.snapshot.resolve()
    audit = json.loads((root / 'audit.json').read_text(encoding='utf-8'))
    for name, digest in audit['source_files'].items():
        with (root / name).open('rb') as f:
            if hashlib.file_digest(f, 'sha256').hexdigest() != digest:
                raise SystemExit(f'Snapshot changed: {name}')
    out = root / 'ordo'
    out.mkdir(exist_ok=False)
    anchors = {'Lynx-1.5.0': 2819, 'Sable-1.1.0': 2933, 'Stash-25.0': 2932}
    anchor_path = out / 'anchors.csv'
    anchor_path.write_text(''.join(f'"{name}",{rating}\n' for name,rating in anchors.items()), encoding='utf-8')
    commands = []
    results = {}
    variants = [('three-anchors', 'rating-input.pgn', ['-m', str(anchor_path)], True),
                ('normal-pairs', 'normal-pairs-input.pgn', ['-m', str(anchor_path)], False)]
    variants += [(f'anchor-{name}', 'rating-input.pgn', ['-A', name, '-a', str(value)], False) for name,value in anchors.items()]
    for name, pgn, anchor_args, errors in variants:
        command = [str(args.ordo.resolve()), '-q', '-M', '-W', '-D', '-n', '1', '-N', '1,2',
                   '-p', str(root / pgn), *anchor_args, '-o', str(out / (name + '.txt')),
                   '-c', str(out / (name + '.csv'))]
        if errors:
            command += ['-s', '1000', '-F', '95']
        commands.append(command)
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n', encoding='utf-8')
        print('START', name, flush=True)
        with (out / (name + '.log')).open('w', encoding='utf-8') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)
        with (out / (name + '.csv')).open(encoding='utf-8', newline='') as f:
            rows = list(csv.reader(f))
        results[name] = [row for row in rows if any('Coco-1.5.0-Dev' in cell for cell in row)]
        print(name, results[name], flush=True)
    results['notes'] = 'Fixed-anchor conditional estimate. Native errors do not include calibration uncertainty or opening-pair dependence. See docs/evidence/gauntlet-rating-method.md.'
    (out / 'summary.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
