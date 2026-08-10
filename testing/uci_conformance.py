#!/usr/bin/env python3
"""Deterministic UCI lifecycle, malformed-input, and race regression checks."""

from __future__ import annotations

import argparse
import queue
import random
import subprocess
import threading
import time
from pathlib import Path


class EngineSession:
    def __init__(self, engine: Path) -> None:
        self.process = subprocess.Popen(
            [str(engine)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1,
            cwd=str(Path(__file__).resolve().parent.parent),
        )
        self.lines: queue.Queue[str | None] = queue.Queue()
        assert self.process.stdout is not None
        threading.Thread(target=self._read_stdout, daemon=True).start()

    def _read_stdout(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            self.lines.put(line.rstrip())
        self.lines.put(None)

    def send(self, *commands: str) -> None:
        assert self.process.stdin is not None
        self.process.stdin.write("".join(command + "\n" for command in commands))
        self.process.stdin.flush()

    def wait_for(self, prefix: str, timeout: float = 10.0) -> list[str]:
        deadline = time.monotonic() + timeout
        observed: list[str] = []
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty as error:
                raise TimeoutError(f"timed out waiting for {prefix!r}; tail={observed[-10:]}") from error
            if line is None:
                stderr = self.process.stderr.read() if self.process.stderr else ""
                raise RuntimeError(f"engine exited before {prefix!r}: {stderr[-2000:]}")
            observed.append(line)
            if line.startswith(prefix):
                return observed
        raise TimeoutError(f"timed out waiting for {prefix!r}")

    def close(self) -> None:
        if self.process.poll() is None:
            self.send("quit")
        self.process.wait(timeout=5)
        if self.process.returncode != 0:
            stderr = self.process.stderr.read() if self.process.stderr else ""
            raise RuntimeError(f"engine exited {self.process.returncode}: {stderr[-2000:]}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("engine", type=Path)
    args = parser.parse_args()
    engine = args.engine.resolve()
    if not engine.is_file():
        parser.error(f"engine not found: {engine}")

    session = EngineSession(engine)
    try:
        session.send("uci")
        uci_lines = session.wait_for("uciok")
        assert any(line == "id name Coco pre-release" for line in uci_lines)
        session.send("setoption name Hash value nonsense", "setoption", "isready")
        session.wait_for("readyok")

        # An invalid replacement must be rejected atomically, preserving startpos.
        session.send("position startpos", "position fen 8/8/8/8/8/8/8/9 w - - 0 1")
        session.wait_for("info string rejected malformed position command")
        session.send("go perft 1")
        perft = session.wait_for("Total nodes:")
        assert perft[-1] == "Total nodes: 20", perft[-10:]

        # Exercise deterministic malformed-FEN and arbitrary-token fuzz without
        # allowing a crash, exception, deadlock, or partial position update.
        malformed = [
            "position", "position fen", "position nope",
            "position startpos garbage", "position startpos moves e2e5",
            "position fen 8/8/8/8/8/8/8/K6k w - - -1 1",
            "position fen 8/8/8/8/8/8/8/K6k w - - 0 999999999999999999999",
        ]
        rng = random.Random(0xC0C015)
        alphabet = "abcdefgh12345678KQkqpnbr!?-/ "
        for _ in range(128):
            payload = "".join(rng.choice(alphabet) for _ in range(rng.randrange(0, 96)))
            malformed.append("position fen " + payload)
        for command in malformed:
            session.send(command)
        session.send("isready")
        session.wait_for("readyok", timeout=15)

        allowed = {"e2e4", "d2d4"}
        session.send("position startpos", "go depth 3 searchmoves e2e4 d2d4 a1a8")
        restricted = session.wait_for("bestmove ")
        assert any(line.startswith("info string rejected illegal searchmoves token a1a8")
                   for line in restricted)
        assert restricted[-1].split()[1] in allowed, restricted[-1]
        for line in restricted:
            if line.startswith("info ") and " pv " in line:
                assert line.split(" pv ", 1)[1].split()[0] in allowed, line

        session.send("setoption name MultiPV value 4", "position startpos",
                     "go searchmoves e2e4 d2d4 depth 3")
        multipv = session.wait_for("bestmove ")
        depth_three = [line for line in multipv if line.startswith("info depth 3 ")]
        assert len(depth_three) == 2, depth_three
        assert {int(line.split(" multipv ", 1)[1].split()[0]) for line in depth_three} == {1, 2}
        assert all(line.split(" pv ", 1)[1].split()[0] in allowed for line in depth_three)

        session.send("setoption name MultiPV value 1", "position startpos",
                     "go depth 2 searchmoves a1a8")
        empty_root = session.wait_for("bestmove ")
        assert empty_root[-1] == "bestmove 0000", empty_root[-10:]

        session.send("position fen 7k/5Q2/6K1/8/8/8/8/8 w - - 0 1", "go mate 1")
        mate = session.wait_for("bestmove ")
        assert any(" score mate 1 " in f" {line} " for line in mate), mate
        assert max(int(line.split()[2]) for line in mate if line.startswith("info depth ")) <= 2

        def signature(lines: list[str]) -> tuple[str, str, str, str]:
            info = [line for line in lines if line.startswith("info depth ")][-1]
            tokens = info.split()
            nodes = tokens[tokens.index("nodes") + 1]
            score_index = tokens.index("score")
            score = " ".join(tokens[score_index + 1:score_index + 3])
            pv = info.split(" pv ", 1)[1]
            bestmove = lines[-1].split()[1]
            return nodes, score, pv, bestmove

        session.send("setoption name Clear Hash", "setoption name UCI_AnalyseMode value false",
                     "position startpos", "go depth 4")
        normal = session.wait_for("bestmove ")
        assert all(" tbhits " in f" {line} " for line in normal if line.startswith("info depth "))
        session.send("setoption name Clear Hash", "setoption name UCI_AnalyseMode value true",
                     "position startpos", "go depth 4")
        analyse = session.wait_for("bestmove ")
        assert signature(normal) == signature(analyse)

        session.send("setoption name Clear Hash", "setoption name UCI_ShowWDL value true",
                     "position startpos", "go depth 4")
        with_wdl = session.wait_for("bestmove ")
        assert signature(normal) == signature(with_wdl)
        for line in with_wdl:
            if not line.startswith("info depth "): continue
            tokens = line.split()
            index = tokens.index("wdl")
            values = [int(value) for value in tokens[index + 1:index + 4]]
            assert all(value >= 0 for value in values) and sum(values) == 1000, line
        session.send("setoption name UCI_ShowWDL value false", "position startpos",
                     "go movetime 8000")
        timed = session.wait_for("bestmove ", timeout=10)
        currmoves = [line for line in timed if " currmove " in line]
        assert currmoves and all(" currmovenumber " in line for line in currmoves), timed[-20:]

        # Stop and ponder transitions must remain responsive under immediate races.
        session.send("position startpos", "go infinite", "stop")
        session.wait_for("bestmove ", timeout=10)
        session.send("position startpos", "go ponder wtime 1000 btime 1000", "ponderhit")
        session.wait_for("bestmove ", timeout=10)
        session.send("isready")
        session.wait_for("readyok")
    finally:
        session.close()

    print("PASS: UCI handshake, malformed input, atomic position parsing, stop, and ponder races")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
