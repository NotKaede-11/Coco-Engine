"""Protocol-level validation for Coco's UCI search limits."""

from __future__ import annotations

import argparse
import re
import subprocess
import time
from pathlib import Path


NODES = re.compile(r"\bnodes (\d+)")


def wait_for(process: subprocess.Popen[str], prefix: str, timeout: float = 10.0) -> list[str]:
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


def run(engine: Path, command: str, threads: int = 1, stop_after: float | None = None) -> tuple[list[str], int]:
    process = subprocess.Popen(
        [str(engine)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
        cwd=str(Path(__file__).resolve().parent.parent),
    )
    assert process.stdin is not None
    process.stdin.write("uci\n")
    process.stdin.flush()
    wait_for(process, "uciok")
    process.stdin.write(f"setoption name Threads value {threads}\n")
    process.stdin.write("isready\n")
    process.stdin.flush()
    wait_for(process, "readyok")
    process.stdin.write("position startpos\n")
    process.stdin.write(command + "\n")
    process.stdin.flush()
    started = time.perf_counter()
    if stop_after is not None:
        time.sleep(stop_after)
        process.stdin.write("stop\n")
        process.stdin.flush()
    lines = wait_for(process, "bestmove ", timeout=20.0)
    elapsed_ms = round((time.perf_counter() - started) * 1000)
    process.stdin.write("quit\n")
    process.stdin.flush()
    process.wait(timeout=5)
    return lines, elapsed_ms


def final_nodes(lines: list[str]) -> int:
    values = [int(match.group(1)) for line in lines if (match := NODES.search(line))]
    if not values:
        raise AssertionError("search emitted no node count")
    return values[-1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("engine", type=Path)
    args = parser.parse_args()
    engine = args.engine.resolve()
    if not engine.is_file():
        parser.error(f"engine not found: {engine}")

    for threads, target in ((1, 1), (1, 257), (1, 10_000), (1, 100_000), (4, 100_000)):
        lines, elapsed = run(engine, f"go nodes {target}", threads=threads)
        observed = final_nodes(lines)
        tolerance = max(16, threads * 128)
        assert target <= observed <= target + tolerance, (threads, target, observed)
        print(f"PASS nodes threads={threads} target={target} observed={observed} wall={elapsed}ms")

    lines, elapsed = run(engine, "go movetime 1000 nodes 1000")
    observed = final_nodes(lines)
    assert 1000 <= observed <= 1128, observed
    assert elapsed < 1000, elapsed
    print(f"PASS combined nodes+movetime observed={observed} wall={elapsed}ms")

    _, short_horizon = run(engine, "go wtime 2000 btime 2000 movestogo 20")
    _, long_horizon = run(engine, "go wtime 2000 btime 2000 movestogo 1")
    assert long_horizon > short_horizon * 1.5, (short_horizon, long_horizon)
    print(f"PASS movestogo mtg20={short_horizon}ms mtg1={long_horizon}ms")

    lines, elapsed = run(engine, "go infinite", stop_after=0.15)
    assert any(line.startswith("bestmove ") for line in lines)
    assert elapsed < 1000, elapsed
    print(f"PASS infinite+stop wall={elapsed}ms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
