#!/usr/bin/env python3
"""Exercise datagen provenance, exact sizing, resume, and mismatch refusal."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command: list[str], expected: int) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(command, text=True, capture_output=True, check=False)
    if completed.returncode != expected:
        raise AssertionError(
            f"expected exit {expected}, got {completed.returncode}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, default=root / "coco-chess.exe")
    args = parser.parse_args()
    engine = args.engine.resolve()
    if not engine.is_file():
        parser.error(f"engine not found: {engine}")

    with tempfile.TemporaryDirectory(prefix="coco-datagen-") as directory:
        output = Path(directory) / "tiny.bin"
        manifest = Path(str(output) + ".manifest.json")
        common = [
            str(engine), "--datagen", "8", "1", str(output),
            "--seed", "20260809", "--buffer", "1", "--datagen-tt", "1",
        ]

        run(common, 0)
        if output.stat().st_size != 8 * 32:
            raise AssertionError("datagen did not stop at exactly eight 32-byte records")
        metadata = json.loads(manifest.read_text(encoding="utf-8"))
        required = {
            "schema": "coco-datagen-manifest-v1",
            "status": "complete",
            "record_schema": "bullet-chessboard-v1",
            "record_bytes": 32,
            "target_records": 8,
            "exact_records": 8,
            "seed": 20260809,
        }
        for key, expected in required.items():
            if metadata.get(key) != expected:
                raise AssertionError(f"manifest {key!r}: expected {expected!r}, got {metadata.get(key)!r}")
        for key in ("configuration_fingerprint", "engine_fnv1a64", "active_nnue"):
            if not str(metadata.get(key, "")).startswith("fnv1a64:"):
                raise AssertionError(f"manifest lacks {key}")

        original_hash = sha256(output)
        run(common, 0)
        if sha256(output) != original_hash:
            raise AssertionError("compatible completed resume changed the dataset")

        incompatible = common.copy()
        incompatible[incompatible.index("20260809")] = "20260810"
        failure = run(incompatible, 1)
        if "Refusing to append" not in failure.stderr:
            raise AssertionError("incompatible resume did not explain its refusal")
        if sha256(output) != original_hash:
            raise AssertionError("incompatible resume changed the dataset")

    print("Datagen provenance test passed (exact output, compatible resume, mismatch refusal).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
