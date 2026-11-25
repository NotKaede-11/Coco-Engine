#!/usr/bin/env python3
"""Verify runtime architecture/compiler/network identity against build inputs."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from pathlib import Path


def fnv1a64(payload: bytes) -> str:
    value = 14695981039346656037
    for byte in payload:
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"fnv1a64:{value:016x}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("engine", type=Path)
    parser.add_argument("--arch", required=True)
    root = Path(__file__).resolve().parents[1]
    config = json.loads((root / 'release.json').read_text())
    parser.add_argument("--network", type=Path, default=root / config.get('nnue_file', 'coco.nnue'))
    args = parser.parse_args()
    engine = args.engine.resolve()
    network = args.network.resolve()
    payload = network.read_bytes()
    process = subprocess.run(
        [str(engine)], input="uci\nquit\n", text=True, capture_output=True,
        cwd=network.parent, timeout=15, check=True,
    )
    lines = [line for line in process.stdout.splitlines()
             if line.startswith("info string build ")]
    assert len(lines) == 1, process.stdout
    line = lines[0]
    expected = {
        "arch": args.arch,
        "embedded_nnue_sha256": hashlib.sha256(payload).hexdigest().upper(),
        "embedded_nnue_bytes": str(len(payload)),
        "active_nnue": ('sha256:' + hashlib.sha256(payload).hexdigest().upper()
                        if config.get('nnue_architecture') == 'ravager-v3' else fnv1a64(payload)),
    }
    for key, value in expected.items():
        assert f"{key}={value}" in line, (key, value, line)
    assert " compiler=" in line and " compiler=unknown" not in line, line
    assert " isa=" in line, line
    print(f"PASS: {line}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
