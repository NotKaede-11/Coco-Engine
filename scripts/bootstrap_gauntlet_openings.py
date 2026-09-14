"""Resample whole opponent/FEN clusters, retaining all repetitions and colours.

This supplements Ordo's independent-game error simulation. Fixed anchors and
the observed opening population remain assumptions, not an official rating.
"""
import argparse
from collections import defaultdict
import csv
import hashlib
import json
from pathlib import Path
import random
import re
import statistics
import subprocess


def quantile(values, p):
    rank = (len(values) - 1) * p
    lo = int(rank)
    hi = min(lo + 1, len(values) - 1)
    return values[lo] + (rank - lo) * (values[hi] - values[lo])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshot', type=Path)
    parser.add_argument('--samples', type=int, default=1000)
    parser.add_argument('--seed', type=int, default=20260914)
    parser.add_argument('--normal-pairs', action='store_true', help='Exclude both games of every pair with an abnormal termination')
    args = parser.parse_args()
    root = args.snapshot.resolve()
    audit = json.loads((root/'audit.json').read_text())
    raw = (root/'gauntlet_games.pgn').read_bytes()
    if hashlib.sha256(raw).hexdigest() != audit['source_files']['gauntlet_games.pgn']:
        raise SystemExit('PGN hash mismatch')
    headers = []
    for block in re.split(r'(?m)(?=^\[Event )', raw.decode('utf-8')):
        if not block.strip():
            continue
        h = dict(re.findall(r'^\[(\w+) "(.*)"\]\r?$', block, re.M))
        headers.append(h)
    def pair_key(h):
        return (tuple(sorted((h['White'],h['Black']))),h['Round'],h['FEN'])
    excluded = {pair_key(h) for h in headers if h['Termination'] != 'normal'} if args.normal_pairs else set()
    clusters = defaultdict(lambda: defaultdict(list))
    for h in headers:
        opponent = h['Black'] if h['White'] == 'Coco-1.5.0-Dev' else h['White']
        if opponent == 'Coco-1.4.0' or pair_key(h) in excluded:
            continue
        text = '\n'.join(f'[{tag} "{h[tag]}"]' for tag in ('White','Black','Result'))+'\n\n'+h['Result']+'\n\n'
        clusters[opponent][h['FEN']].append(text)
    out = root/'ordo'/('normal-pairs-opening-bootstrap' if args.normal_pairs else 'opening-bootstrap')
    out.mkdir(exist_ok=False)
    pgn = out/'sample.pgn'
    csv_path = out/'sample.csv'
    command = [str(Path('tools/ordo/ordo.exe').resolve()), '-q','-M','-W','-D','-n','1','-N','2,2',
               '-p',str(pgn),'-m',str(root/'ordo/anchors.csv'),'-o',str(out/'sample.txt'),'-c',str(csv_path)]
    rng = random.Random(args.seed)
    populations = {name:[''.join(games) for games in groups.values()] for name,groups in sorted(clusters.items())}
    ratings = []
    for index in range(args.samples):
        pgn.write_text(''.join(rng.choice(pop) for pop in populations.values() for _ in range(len(pop))),encoding='utf-8')
        subprocess.run(command,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,timeout=30)
        rows = list(csv.reader(csv_path.open(newline='',encoding='utf-8')))
        row = next(row for row in rows if 'Coco-1.5.0-Dev' in row)
        ratings.append(float(row[2]))
        if (index+1)%100==0:
            print(f'Bootstrap {index+1}/{args.samples}',flush=True)
    ordered = sorted(ratings)
    result = {'samples':args.samples,'seed':args.seed,'resampling_unit':'opponent + initial FEN; every repeated colour pair retained',
              'normal_pairs_only':args.normal_pairs,'input_games':sum(len(g) for v in clusters.values() for g in v.values()),
              'clusters_per_opponent':{k:len(v) for k,v in clusters.items()},
              'games_per_cluster':{k:sorted(set(len(g) for g in v.values())) for k,v in clusters.items()},
              'mean':statistics.mean(ratings),'sd':statistics.stdev(ratings),
              'percentile_95':[quantile(ordered,.025),quantile(ordered,.975)],'command':command,
              'limitation':'Conditional on fixed anchors and sampled opening population; does not cover time-control transfer or systematic run defects.'}
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    (out/'ratings.json').write_text(json.dumps(ratings)+'\n',encoding='utf-8')
    print(json.dumps(result,indent=2),flush=True)


if __name__ == '__main__':
    main()
