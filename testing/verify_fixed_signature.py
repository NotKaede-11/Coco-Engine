#!/usr/bin/env python3
"""Assert Coco's deterministic five-position benchmark node signature."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("engine", type=Path)
    metadata = json.loads((Path(__file__).resolve().parents[1] / "release.json").read_text())
    parser.add_argument("--nodes", type=int, default=metadata["fixed_signature_nodes"])
    args = parser.parse_args()
    engine = args.engine.resolve()
    result = subprocess.run(
        [str(engine)], input="bench\nquit\n", text=True, capture_output=True,
        cwd=engine.parent, timeout=90, check=True,
    )
    matches = re.findall(r"Total nodes searched:\s*(\d+)", result.stdout)
    assert matches and int(matches[-1]) == args.nodes, (matches, result.stdout[-4000:])
    print(f"PASS: fixed five-position signature = {args.nodes} nodes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
