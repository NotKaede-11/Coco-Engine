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
    completed = subprocess.run(command, text=True, capture_output=True, check=False, timeout=120)
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

    (root / "scratch").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="coco-datagen-", dir=root / "scratch") as directory:
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
            "repetition_policy": "game_threefold_legal_ep_v1",
            "rule50_policy": "pawn_capture_reset_v1",
        }
        for key, expected in required.items():
            if metadata.get(key) != expected:
                raise AssertionError(f"manifest {key!r}: expected {expected!r}, got {metadata.get(key)!r}")
        for key in ("configuration_fingerprint", "engine_fnv1a64"):
            if not str(metadata.get(key, "")).startswith("fnv1a64:"):
                raise AssertionError(f"manifest lacks {key}")
        active = str(metadata.get("active_nnue", ""))
        if active.startswith("sha256:"):
            release = json.loads((root / "release.json").read_text(encoding="utf-8"))
            assert active == "sha256:" + release["nnue_sha256"]
            assert metadata["embedded_nnue_sha256"] == release["nnue_sha256"]
        else:
            assert active.startswith("fnv1a64:"), metadata

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

        # Restarting the random stream is not an exact continuation.
        extension = common.copy()
        extension[2] = "16"
        failure = run(extension, 1)
        assert "saved RNG state" in failure.stderr
        assert sha256(output) == original_hash

        changed_depth = common + ["--datagen-depth", "5"]
        assert "Refusing to append" in run(changed_depth, 1).stderr
        assert sha256(output) == original_hash

        # Structurally aligned but damaged data must fail even at completed target.
        original = output.read_bytes()
        for offset, value in ((0, b'\0' * 8), (26, b'\x03'), (27, b'\xff')):
            output.write_bytes(original[:offset] + value + original[offset + len(value):])
            assert "Refusing invalid existing data" in run(common, 1).stderr
        output.write_bytes(original)

        # High FEN fullmove numbers must not immediately exhaust the move cap.
        # A one-ply cap without adjudication/terminal result must not create draws.
        book = Path(directory) / "cap.fen"
        book.write_text("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 150\n")
        capped = Path(directory) / "capped.bin"
        command = [str(engine), "--datagen", "1", "1", str(capped),
                   "--book", str(book), "--datagen-depth", "1",
                   "--max-game-ply", "1", "--max-empty-games", "2", "--datagen-tt", "1"]
        failure = run(command, 1)
        assert "without usable records" in failure.stderr
        assert capped.stat().st_size == 0
        capped_meta = json.loads(Path(str(capped) + ".manifest.json").read_text())
        assert capped_meta["status"] == "failed"
        assert capped_meta["capped_games_this_session"] == 2
        assert capped_meta["search"]["main_depth"] == 1
        assert capped_meta["capped_game_policy"] == "discard_unresolved"

        # Configuration errors must not create or alter output.
        for value in ("0", "17"):
            assert "Invalid" in run(command + ["--datagen-depth", value], 1).stderr
        assert capped.stat().st_size == 0

    print("Datagen tests passed: sizing, provenance, depth identity, unsafe extension refusal, corruption and capped-game handling.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
