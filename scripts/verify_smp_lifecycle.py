#!/usr/bin/env python3
"""Exercise repeated Lazy-SMP start/finish/stop/ponder transitions."""

from __future__ import annotations

import argparse
import subprocess
import time
from pathlib import Path


START_MOVES = {
    "a2a3", "a2a4", "b2b3", "b2b4", "c2c3", "c2c4", "d2d3",
    "d2d4", "e2e3", "e2e4", "f2f3", "f2f4", "g2g3", "g2g4",
    "h2h3", "h2h4", "b1a3", "b1c3", "g1f3", "g1h3",
}


def wait_for(process: subprocess.Popen[str], prefix: str, timeout: float = 15.0) -> list[str]:
    deadline = time.monotonic() + timeout
    lines: list[str] = []
    assert process.stdout is not None
    while time.monotonic() < deadline:
        line = process.stdout.readline()
        if not line:
            stderr = process.stderr.read() if process.stderr is not None else ""
            raise RuntimeError(f"engine exited before {prefix!r}: {stderr[-1000:]}")
        line = line.strip()
        lines.append(line)
        if line.startswith(prefix):
            return lines
    raise TimeoutError(f"timed out waiting for {prefix!r}")


def bestmove(lines: list[str]) -> str:
    return next(line.split()[1] for line in reversed(lines)
                if line.startswith("bestmove "))


def run_threads(engine: Path, threads: int) -> None:
    process = subprocess.Popen(
        [str(engine)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, bufsize=1, cwd=str(engine.parent),
    )
    assert process.stdin is not None
    try:
        process.stdin.write("uci\n")
        process.stdin.flush()
        wait_for(process, "uciok")
        process.stdin.write(f"setoption name Threads value {threads}\n")
        process.stdin.write("setoption name Ponder value true\n")
        process.stdin.write("isready\n")
        process.stdin.flush()
        wait_for(process, "readyok")

        for _ in range(4):
            process.stdin.write("ucinewgame\nposition startpos\ngo nodes 20000\n")
            process.stdin.flush()
            move = bestmove(wait_for(process, "bestmove "))
            if move not in START_MOVES:
                raise AssertionError((threads, "illegal start move", move))

        process.stdin.write("position startpos\ngo infinite\n")
        process.stdin.flush()
        time.sleep(0.05)
        process.stdin.write("stop\n")
        process.stdin.flush()
        if bestmove(wait_for(process, "bestmove ")) not in START_MOVES:
            raise AssertionError((threads, "illegal stop fallback"))

        process.stdin.write("position startpos\ngo ponder wtime 1000 btime 1000\n")
        process.stdin.flush()
        time.sleep(0.05)
        process.stdin.write("ponderhit\n")
        process.stdin.flush()
        if bestmove(wait_for(process, "bestmove ")) not in START_MOVES:
            raise AssertionError((threads, "illegal ponder result"))

        process.stdin.write("isready\n")
        process.stdin.flush()
        wait_for(process, "readyok")
    finally:
        if process.poll() is None:
            process.stdin.write("quit\n")
            process.stdin.flush()
            process.wait(timeout=10)
    print(f"PASS: repeated search/stop/ponder lifecycle with Threads={threads}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("engine", type=Path)
    args = parser.parse_args()
    engine = args.engine.resolve()
    if not engine.is_file():
        parser.error(f"engine not found: {engine}")
    for threads in (2, 4, 8):
        run_threads(engine, threads)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
