#!/usr/bin/env python3
"""Guard tactical/noisy openings extracted from t5_overhaulv1.pgn.

Acceptable root moves were cross-checked with local Stockfish depth 16 and the
SPRT-accepted Phase 8 Coco binary at deterministic depth 10. The test is broad
enough to permit equivalent choices but rejects the old DEV tactical failures.
"""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


CASES = [
    {
        "name": "avoid passive Bd7 after structural exchange",
        "fen": "rnbqk2r/pppp2pp/4pn2/5p2/1b3P2/2NP1N2/PPP1P1PP/R1BQKB1R w KQkq - 0 5",
        "moves": ["g2g3", "d7d5", "f1g2", "b4c3", "b2c3"],
        "acceptable": {"c7c5", "e8g8", "b8c6", "d8e7", "b7b6"},
    },
    {
        "name": "avoid unsupported Nb5",
        "fen": "rnbqkb1r/p1pp1p1p/1p2pnp1/8/P4P2/4P3/1PPPB1PP/RNBQK1NR w KQkq - 0 5",
        "moves": ["b1c3", "c7c5", "e2f3", "d7d5", "d2d3", "b8c6"],
        "acceptable": {"g1e2", "g1h3", "e3e4", "h2h4"},
    },
    {
        "name": "retreat instead of Nc7 material loss",
        "fen": "rnbqkb1r/p1pp1p1p/1p2pnp1/8/P4P2/4P3/1PPPB1PP/RNBQK1NR w KQkq - 0 5",
        "moves": ["b1c3", "c7c5", "e2f3", "d7d5", "d2d3", "b8c6", "c3b5", "f8g7"],
        "acceptable": {"e3e4", "g1h3", "g1e2", "c2c3"},
    },
    {
        "name": "develop instead of Nb5/Nxd6 sequence",
        "fen": "rnbqkb1r/p2pnppp/1p2p3/2p5/2P4P/N2P4/PP2PPP1/R1BQKBNR w KQkq - 0 5",
        "moves": [],
        "acceptable": {"g1f3", "e2e4", "g2g3", "h4h5"},
    },
    {
        "name": "avoid premature Qe4 queen tactic",
        "fen": "rnb1k1nr/p1pp1ppp/1p1b1q2/4p3/P5Q1/2P1P3/1P1P1PPP/RNB1KBNR w KQkq - 0 5",
        "moves": [],
        "acceptable": {"a4a5", "d2d4", "d2d3", "g1f3", "e3e4"},
    },
]


def wait_for(process: subprocess.Popen[str], marker: str) -> None:
    assert process.stdout is not None
    while True:
        line = process.stdout.readline()
        if not line:
            raise RuntimeError(f"engine exited before {marker}")
        if line.strip() == marker:
            return


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--depth", type=int, default=10)
    args = parser.parse_args()
    engine = args.engine.resolve()

    process = subprocess.Popen(
        [str(engine)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, bufsize=1, cwd=str(engine.parent.parent)
    )
    assert process.stdin is not None
    assert process.stdout is not None

    process.stdin.write("uci\nisready\n")
    process.stdin.flush()
    wait_for(process, "uciok")
    wait_for(process, "readyok")

    failures = []
    for case in CASES:
        suffix = " moves " + " ".join(case["moves"]) if case["moves"] else ""
        process.stdin.write("ucinewgame\n")
        process.stdin.write(f"position fen {case['fen']}{suffix}\n")
        process.stdin.write(f"go depth {args.depth}\n")
        process.stdin.flush()

        bestmove = ""
        while True:
            line = process.stdout.readline().strip()
            if line.startswith("bestmove "):
                bestmove = line.split()[1]
                break
        passed = bestmove in case["acceptable"]
        print(f"{'PASS' if passed else 'FAIL'}: {case['name']}: {bestmove}")
        if not passed:
            failures.append((case["name"], bestmove, sorted(case["acceptable"])))

    process.stdin.write("quit\n")
    process.stdin.flush()
    process.wait(timeout=5)
    if failures:
        for name, move, acceptable in failures:
            print(f"  {name}: got {move}, expected one of {acceptable}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
