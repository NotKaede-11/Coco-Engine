#!/usr/bin/env python3
"""Shared-folder startup and explicit NNUE reload regression (real UCI process)."""
import argparse
import hashlib
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
from pathlib import Path
from uci_conformance import EngineSession
from verify_build_identity import fnv1a64


class Session(EngineSession):
    def __init__(self, engine, cwd):
        self.process = subprocess.Popen([str(engine)], cwd=cwd, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        self.lines = queue.Queue()
        threading.Thread(target=self._read_stdout, daemon=True).start()


def identity(session):
    session.send('uci')
    lines = session.wait_for('uciok')
    assert 'id name Coco v1.5.1' in lines, lines
    assert 'option name EvalFile type string default <embedded>' in lines, lines
    return next(line for line in lines if line.startswith('info string build '))


def load(session, name, success):
    session.send('setoption name EvalFile value '+name, 'isready')
    lines = session.wait_for('readyok')
    assert any(('loaded successfully' if success else 'Keeping the current network') in line
               for line in lines), lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('engine', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    payload = (root/'coco.nnue').read_bytes()
    embedded = fnv1a64(payload)
    with tempfile.TemporaryDirectory(prefix='nnue-loading-', dir=root) as temp:
        base = Path(temp)
        bin_dir = base/'shared engines';bin_dir.mkdir()
        work = base/'gui working directory';work.mkdir()
        engine = bin_dir/args.engine.name
        shutil.copy2(args.engine.resolve(), engine)
        # Old-size file, truncated file and a different same-size network must
        # all be ignored at startup, in both supported ambient search locations.
        for directory in (work, bin_dir):
            for data in (bytes(394756), b'broken', bytes(len(payload))):
                legacy = directory/'coco.nnue';legacy.write_bytes(data)
                before = hashlib.sha256(legacy.read_bytes()).hexdigest()
                session = Session(engine, work)
                try:
                    assert 'active_nnue='+embedded in identity(session)
                    session.send('isready');session.wait_for('readyok')
                    session.send('position startpos', 'go depth 3')
                    assert session.wait_for('bestmove ')[-1].split()[1] != '0000'
                finally:
                    session.close()
                assert hashlib.sha256(legacy.read_bytes()).hexdigest() == before
                legacy.unlink()

        # A stale GUI option must report rejection without aborting. Explicit
        # reloads preserve the position and reset accumulators for the new net.
        (work/'coco.nnue').write_bytes(b'old network')
        custom = work/'custom network.nnue';custom.write_bytes(bytes(len(payload)))
        named = bin_dir/'coco-512x2-392be46c8e06.nnue';named.write_bytes(payload)
        session = Session(engine, work)
        try:
            assert 'active_nnue='+embedded in identity(session)
            load(session,'coco.nnue',False)
            assert 'active_nnue='+embedded in identity(session)
            session.send('position startpos moves e2e4 e7e5 g1f3', 'go perft 2')
            original_perft = session.wait_for('Total nodes:')[-1]
            load(session, str(custom), True)
            custom_id = fnv1a64(custom.read_bytes())
            assert custom_id != embedded and 'active_nnue='+custom_id in identity(session)
            for name in ('missing.nnue','coco.nnue'):
                load(session,name,False)
                assert 'active_nnue='+custom_id in identity(session)
            # <embedded> must actively restore weights, not just return success.
            load(session,'<embedded>',True)
            assert 'active_nnue='+embedded in identity(session)
            session.send('go perft 2')
            assert session.wait_for('Total nodes:')[-1] == original_perft
            load(session, str(named) if sys.platform == "darwin" else named.name, True)
            assert 'active_nnue='+embedded in identity(session)
            session.send('setoption name Threads value 2', 'go infinite')
            session.wait_for('info depth ')
            load(session,'<embedded>',True)  # stop/join before changing weights
            session.send('go depth 4')
            assert session.wait_for('bestmove ')[-1].split()[1] != '0000'
            session.send('setoption name Threads value 1', 'bench')
            assert session.wait_for('Total nodes searched:', timeout=90)[-1] == 'Total nodes searched: 477035'
        finally:
            session.close()
    print('PASS: six ambient collisions; explicit valid/missing/incompatible reloads; '
          'embedded restore, parent position, space paths and threaded reload')


if __name__ == '__main__':
    main()
