#!/usr/bin/env python3
"""Check the public UCI interface and option handshake with a bounded timeout."""
import argparse
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("engine", type=Path)
    parser.add_argument("--expected-name", default=json.loads((ROOT / "release.json").read_text())["engine_name"])
    args = parser.parse_args()
    engine = args.engine.resolve()
    commands = """uci
debug on
isready
debug off
setoption name Move Overhead value 45
setoption name SyzygyProbeLimit value false
setoption name SyzygyProbeLimit value true
setoption name Syzygy50MoveRule value false
setoption name Syzygy50MoveRule value true
setoption name UCI_AnalyseMode value true
setoption name UCI_AnalyseMode value false
setoption name NMP_Base value 4
setoption name NMP_Base value 3
setoption name SEE_Pruning_Depth value 0
setoption name EvalFile value coco.nnue
isready
quit
"""
    result = subprocess.run([str(engine)], input=commands, text=True, capture_output=True,
                            cwd=ROOT, timeout=30, check=True)
    lines = result.stdout.splitlines()
    assert f"id name {args.expected_name}" in lines, result.stdout
    assert lines.count("uciok") == 1 and lines.count("readyok") == 2, result.stdout
    expected = {
        "Hash", "Clear Hash", "Threads", "Ponder", "MultiPV", "Move Overhead", "Use PEXT",
        "EvalFile", "SyzygyPath", "SyzygyProbeDepth", "SyzygyProbeLimit", "Syzygy50MoveRule",
        "UCI_ShowWDL", "UCI_AnalyseMode", "Contempt",
    }
    options = [line for line in lines if line.startswith("option name ")]
    names = [re.fullmatch(r"option name (.+?) type .+", line).group(1) for line in options]
    assert len(names) == len(expected) and set(names) == expected, options
    for option in ("SyzygyProbeLimit type check default true", "Syzygy50MoveRule type check default true",
                   "UCI_ShowWDL type check default false", "UCI_AnalyseMode type check default false",
                   "Move Overhead type spin default 30 min 0 max 5000",
                   "EvalFile type string default <embedded>"):
        assert "option name " + option in options, options
    assert "info string Debug mode on" in lines, result.stdout
    assert any("loaded" in line.lower() and ("network" in line.lower() or "nnue" in line.lower())
               for line in lines), result.stdout
    assert not any("unknown option" in line.lower() for line in lines), result.stdout
    print("PASS: version, fifteen public options, debug/readiness and NNUE reload handshake")
    print("Internal setters exercised; this handshake test does not measure their search effects.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
