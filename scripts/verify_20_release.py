"""Validate standalone embedding, search parity and the complete FRC oracle suite."""
import argparse
import contextlib
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

import chess

ROOT = Path(__file__).resolve().parents[1]
from uci_conformance import EngineSession
from chess960_oracle import verify_variant, exchange, divide


def signature(lines):
    result = []
    for line in lines:
        if line.startswith('bestmove '):
            result.append(line)
        elif line.startswith('info depth ') and ' pv ' in line:
            result.append(tuple(re.findall(
                r'\b(?:depth|seldepth|multipv|nodes) \d+|\bscore (?:cp|mate) -?\d+(?: (?:lowerbound|upperbound))?|\bpv .*', line)))
    return result


class OracleEngine(EngineSession):
    def send(self, commands):
        super().send(commands)

    def receive(self, prefix, timeout=30):
        return self.wait_for(prefix, timeout)


def parity(engine_path, control):
    sample = json.loads((ROOT / 'scripts/search-parity.json').read_text())
    results = []
    a, b = EngineSession(control), EngineSession(engine_path)
    try:
        for e in (a, b):
            e.send('uci'); e.wait_for('uciok')
            e.send('setoption name Hash value 16', 'setoption name Threads value 1',
                   'setoption name Use PEXT value true', 'isready'); e.wait_for('readyok')
        for case in sample['cases']:
            signatures = []
            for e in (a, b):
                e.send('ucinewgame', f"setoption name UCI_Chess960 value {str(case.get('chess960', False)).lower()}", 'isready')
                e.wait_for('readyok')
                if case['warm']:
                    board = chess.Board(case['root_fen'])
                    for move in case['moves']: board.push_uci(move)
                    for back in (4, 2):
                        prior = board.copy()
                        for _ in range(min(back, len(prior.move_stack))): prior.pop()
                        e.send('position fen ' + prior.fen(), 'go nodes 30000'); e.wait_for('bestmove ')
                e.send('position fen ' + case['root_fen'] + (' moves ' + ' '.join(case['moves']) if case['moves'] else ''),
                       'go nodes 100000')
                lines = e.wait_for('bestmove ', timeout=45)
                board = chess.Board(case['root_fen'], chess960=case.get('chess960', False))
                for move in case['moves']: board.push_uci(move)
                token = lines[-1].split()[1]
                if token != '0000': assert board.parse_uci(token) in board.legal_moves
                signatures.append(signature(lines))
            assert signatures[0] == signatures[1], (case['id'], signatures)
            results.append({'id': case['id'], 'signature': signatures[1]})
        return results
    finally:
        a.close(); b.close()


def standalone(binary):
    metadata = json.loads((ROOT / 'release.json').read_text())
    (ROOT / 'scratch').mkdir(exist_ok=True)
    payload = (ROOT / metadata['nnue_file']).read_bytes()
    artifact = binary.read_bytes()
    offset = artifact.find(payload[:4096])
    assert offset >= 0 and artifact.find(payload[:4096], offset + 1) == -1
    assert hashlib.sha256(artifact[offset:offset + len(payload)]).hexdigest().upper() == metadata['nnue_sha256']
    with tempfile.TemporaryDirectory(dir=ROOT / 'scratch') as temp:
        folder = Path(temp); executable = folder / binary.name
        shutil.copy2(binary, executable)
        other = folder / 'unrelated working directory'; other.mkdir()
        expected = 'active_nnue=sha256:' + metadata['nnue_sha256']
        for stale in (False, True):
            if stale:
                (folder / 'coco.nnue').write_bytes(b'wrong legacy weights')
                (other / 'coco.nnue').write_bytes(b'wrong working-directory weights')
                (folder / 'Ravager_NET.nnue').write_bytes(b'wrong donor weights')
            p = subprocess.run([str(executable)], input='uci\nisready\nposition startpos\ngo perft 2\nquit\n',
                               cwd=other, text=True, capture_output=True, check=True, timeout=30)
            assert expected in p.stdout and 'Total nodes: 400' in p.stdout, p.stdout
            assert 'active_source=embedded:ravager-v3' in p.stdout
            assert not p.stderr, p.stderr
        process = EngineSession(executable, cwd=other)
        try:
            process.send('uci'); process.wait_for('uciok')
            process.send('position startpos', 'go nodes 1000')
            best = process.wait_for('bestmove ')[-1].split()[1]
            assert chess.Board().parse_uci(best) in chess.Board().legal_moves
        finally:
            process.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('engine', type=Path)
    parser.add_argument('--control', type=Path)
    parser.add_argument('--frc', action='store_true')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args(); binary = args.engine.resolve()
    report = {'engine_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(), 'oracle_version': chess.__version__}
    standalone(binary); report['standalone_and_stale_file_checks'] = 'PASS'
    print('PASS: standalone embedded startup, unrelated cwd, stale-file isolation and legal search.', flush=True)
    if args.control:
        report['parity'] = parity(binary, args.control.resolve())
        print(f"PASS: {len(report['parity'])} fixed-node parity cases, including warm cache and FRC.", flush=True)
    if args.frc:
        engine = OracleEngine(binary)
        try:
            exchange(engine, 'uci\nsetoption name Threads value 1\nsetoption name Hash value 16')
            for backend in (False, True): verify_variant(engine, backend)
            exchange(engine, 'setoption name UCI_Chess960 value false')
            for fen in ('rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1',
                        'r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1'):
                divide(engine, chess.Board(fen), 3)
            report['frc'] = {'backends': 2, 'starts_per_backend': 960, 'cleared_starts_per_backend': 960,
                             'castling_replay_searchmoves_pv_hash_eval_restore': 'PASS'}
        finally:
            engine.close()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
