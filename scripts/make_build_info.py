#!/usr/bin/env python3
"""Generate deterministic compile-time artifact and embedded-network identity."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", default="native")
    parser.add_argument("--compiler", default="gcc")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    network = root / "coco.nnue"
    payload = network.read_bytes()
    compiler = args.compiler or "gcc"
    rendered = "\n".join([
        "#ifndef COCO_BUILD_INFO_H",
        "#define COCO_BUILD_INFO_H",
        f"#define COCO_BUILD_ARCH {json.dumps(args.arch)}",
        f"#define COCO_BUILD_REQUESTED_COMPILER {json.dumps(compiler)}",
        f"#define COCO_EMBEDDED_NNUE_SHA256 {json.dumps(hashlib.sha256(payload).hexdigest().upper())}",
        f"#define COCO_EMBEDDED_NNUE_SIZE {len(payload)}ULL",
        "#endif",
        "",
    ])
    destination = root / "src" / "build_info.h"
    destination.write_text(rendered, encoding="utf-8", newline="\n")
    print(f"Generated {destination.name}: arch={args.arch} compiler={compiler} "
          f"nnue_sha256={hashlib.sha256(payload).hexdigest().upper()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
