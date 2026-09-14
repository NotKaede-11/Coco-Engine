"""Freeze a completed Coco gauntlet and generate reproducible Ordo inputs.

Read-only toward the live run. Refuses partial totals, mismatched config counts,
incomplete games, or broken colour pairs. Output must be a new directory.
"""
import argparse
from collections import Counter, defaultdict
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil

CANDIDATE = 'Coco-1.5.0-Dev'
HISTORICAL = 'Coco-1.4.0'


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-dir', type=Path, default=Path('tournament_settings'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expected-games', type=int, default=5000)
    args = parser.parse_args()
    cfg_file = args.run_dir / 'gauntlet_config.json'
    pgn_file = args.run_dir / 'gauntlet_games.pgn'
    cfg_bytes = cfg_file.read_bytes()
    cfg = json.loads(cfg_bytes)
    cfg_count = sum(s['wins'] + s['losses'] + s['draws'] for s in cfg['stats'].values())
    if cfg_count != args.expected_games:
        raise SystemExit(f'Not complete: config has {cfg_count}/{args.expected_games} games')
    raw = pgn_file.read_bytes()
    blocks = [b for b in re.split(r'(?m)(?=^\[Event )', raw.decode('utf-8')) if b.strip()]
    if len(blocks) != args.expected_games:
        raise SystemExit(f'PGN has {len(blocks)}, expected {args.expected_games}')
    groups = defaultdict(list)
    results = defaultdict(Counter)
    terminations = Counter()
    abnormal = []
    headers = []
    for i, block in enumerate(blocks, 1):
        h = dict(re.findall(r'^\[(\w+) "(.*)"\]\r?$', block, re.M))
        if CANDIDATE not in (h['White'], h['Black']):
            raise ValueError(f'Game {i}: candidate absent')
        result = h['Result']
        if result not in ('1-0', '0-1', '1/2-1/2') or not block.rstrip().endswith(result):
            raise ValueError(f'Game {i}: incomplete or mismatched result')
        opponent = h['Black'] if h['White'] == CANDIDATE else h['White']
        outcome = 'D' if result == '1/2-1/2' else ('W' if (result == '1-0') == (h['White'] == CANDIDATE) else 'L')
        results[opponent][outcome] += 1
        terminations[h['Termination']] += 1
        key = (opponent, h['Round'], h['FEN'])
        groups[key].append((h, outcome))
        headers.append((h, key))
        if h['Termination'] != 'normal':
            abnormal.append({'game': i, 'opponent': opponent, 'round': h['Round'],
                             'white': h['White'], 'black': h['Black'], 'result': result,
                             'termination': h['Termination'], 'coco_outcome': outcome,
                             'ended': h.get('GameEndTime'), 'tail': block[-350:]})
    for pair, stat in cfg['stats'].items():
        a, b = pair.split(' vs ')
        if CANDIDATE not in (a, b):
            if sum(stat[k] for k in ('wins', 'losses', 'draws')):
                raise ValueError('Unexpected opponent-versus-opponent results')
            continue
        opponent = b if a == CANDIDATE else a
        expected = {'W': stat['wins' if a == CANDIDATE else 'losses'],
                    'L': stat['losses' if a == CANDIDATE else 'wins'], 'D': stat['draws']}
        if any(results[opponent][k] != v for k, v in expected.items()):
            raise ValueError(f'Config/PGN mismatch: {opponent}')
    pair_bins = defaultdict(Counter)
    abnormal_keys = set()
    for key, pair in groups.items():
        if len(pair) != 2 or pair[0][0]['White'] != pair[1][0]['Black'] or pair[0][0]['Black'] != pair[1][0]['White']:
            raise ValueError(f'Broken colour pair: {key[:2]}')
        score = sum({'W': 2, 'D': 1, 'L': 0}[outcome] for h, outcome in pair)
        pair_bins[key[0]][score] += 1
        if any(h['Termination'] != 'normal' for h, outcome in pair):
            abnormal_keys.add(key)
    # Check that the live inputs did not change during the audit.
    if cfg_file.read_bytes() != cfg_bytes or sha256(pgn_file) != hashlib.sha256(raw).hexdigest():
        raise SystemExit('Run changed during audit; retry after completion')
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'gauntlet_config.json').write_bytes(cfg_bytes)
    (args.output / 'gauntlet_games.pgn').write_bytes(raw)
    log = args.run_dir / 'fastchess_gauntlet.log'
    shutil.copy2(log, args.output / log.name)
    # Ordo only needs player/result tags. Full replayable PGN is retained above.
    for label, clean in [('rating-input', False), ('normal-pairs-input', True)]:
        selected = [h for h,key in headers if key[0] != HISTORICAL and (not clean or key not in abnormal_keys)]
        with (args.output / (label + '.pgn')).open('w', encoding='utf-8') as out:
            for h in selected:
                for tag in ('Event', 'Site', 'Date', 'Round', 'White', 'Black', 'Result'):
                    out.write(f'[{tag} "{h[tag]}"]\n')
                out.write('\n' + h['Result'] + '\n\n')
    identities = []
    for engine in cfg['engines']:
        exe = Path(engine['cmd'])
        item = {'name': engine['name'], 'path': str(exe), 'bytes': exe.stat().st_size, 'sha256': sha256(exe)}
        net = Path(engine['dir']) / 'coco.nnue'
        if net.is_file():
            item['external_coco_nnue'] = {'bytes': net.stat().st_size, 'sha256': sha256(net)}
        identities.append(item)
    audit = {'captured_utc': datetime.now(timezone.utc).isoformat(), 'games': len(blocks),
             'config_results_match': True, 'colour_pairs': len(groups),
             'distinct_opponent_fens': len({(key[0], key[2]) for key in groups}),
             'results': dict(results), 'pair_bins_LL_LD_mid_WD_WW': {k:[v[i] for i in range(5)] for k,v in pair_bins.items()},
             'terminations': dict(terminations), 'abnormal': abnormal,
             'abnormal_pairs_excluding_historical': sum(k[0] != HISTORICAL for k in abnormal_keys),
             'engine_identities': identities,
             'source_files': {p.name: sha256(args.output / p.name) for p in (cfg_file,pgn_file,log)},
             'opening': {'path': cfg['opening']['file'], 'sha256': sha256(Path(cfg['opening']['file']))},
             'runner_sha256': sha256(Path('testing/fastchess.exe'))}
    (args.output / 'audit.json').write_text(json.dumps(audit, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k:audit[k] for k in ('games','colour_pairs','distinct_opponent_fens','results','terminations','abnormal_pairs_excluding_historical')},indent=2))


if __name__ == '__main__':
    main()
