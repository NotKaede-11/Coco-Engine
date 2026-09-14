#!/usr/bin/env python3
"""Verify local release metadata; --ready is required before creating a release draft."""
import argparse
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def validate(root=ROOT, *, ready=False, tag=None):
    config = json.loads((root / 'release.json').read_text(encoding='utf-8'))
    if not re.fullmatch(r'\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?', config['version']):
        raise ValueError('Invalid release version')
    if config['engine_name'] != 'Coco v' + config['version']:
        raise ValueError('Engine name and release version disagree')
    if tag is not None and tag != 'v' + config['version']:
        raise ValueError(f'Tag {tag} does not match prepared v{config["version"]}')
    network = (root / 'coco.nnue').read_bytes()
    if len(network) != config['nnue_bytes'] or hashlib.sha256(network).hexdigest().upper() != config['nnue_sha256']:
        raise ValueError('Release network hash/size mismatch')
    if config['nnue_bytes'] != 2 * 771 * config['nnue_l1_size'] + 4:
        raise ValueError('Network dimensions disagree with file size')
    if f'id name {config["engine_name"]}\\n' not in (root / 'src/uci.cpp').read_text():
        raise ValueError('Source UCI identity does not match release metadata')
    notes = root / config['notes']
    if not notes.resolve().is_relative_to(root.resolve()) or not notes.is_file():
        raise ValueError('Missing release notes')
    if not isinstance(config['fixed_signature_nodes'], int) or config['fixed_signature_nodes'] <= 0:
        raise ValueError('Missing deterministic signature')
    artifacts = config['artifacts']
    if (len(artifacts) != 13 or len(set(artifacts)) != 13
            or any(not re.fullmatch(r'coco-chess-[a-z0-9.-]+', name) for name in artifacts)):
        raise ValueError('Expected thirteen distinct platform artifact names')
    checks = config['readiness_checks']
    required = {'sprt_log_reviewed', 'gauntlet_completed_and_reviewed',
                'historical_opponent_provenance_reviewed', 'release_platform_checks_passed'}
    if set(checks) != required or any(type(v) is not bool for v in checks.values()):
        raise ValueError('Invalid release readiness checklist')
    if ready and (config['release_ready'] is not True or not all(checks.values())):
        raise ValueError('Release is not ready: ' + ', '.join(k for k, v in checks.items() if not v))
    return config

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ready', action='store_true')
    parser.add_argument('--tag')
    args = parser.parse_args()
    try:
        config = validate(ready=args.ready, tag=args.tag)
    except (ValueError, KeyError, OSError) as error:
        parser.exit(1, f'FAIL: {error}\n')
    print(f'PASS: v{config["version"]} metadata; release_ready={config["release_ready"]}')

if __name__ == '__main__':
    main()
