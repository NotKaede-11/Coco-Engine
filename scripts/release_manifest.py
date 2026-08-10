#!/usr/bin/env python3
"""Create per-artifact provenance and assemble a verified release manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def runtime_identity(engine: Path) -> str:
    process = subprocess.run(
        [str(engine.resolve())], input="uci\nquit\n", text=True,
        capture_output=True, cwd=engine.resolve().parent, timeout=20, check=True,
    )
    lines = [line for line in process.stdout.splitlines()
             if line.startswith("info string build ")]
    if len(lines) != 1:
        raise RuntimeError(f"missing unique runtime identity in {engine}: {process.stdout}")
    return lines[0].removeprefix("info string build ")


def write_json(path: Path, payload: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8", newline="\n")


def create_artifact(args: argparse.Namespace) -> int:
    artifact = args.artifact.resolve()
    network = args.network.resolve()
    payload = {
        "schema": "coco-release-artifact-v1",
        "artifact": artifact.name,
        "artifact_sha256": sha256(artifact),
        "artifact_bytes": artifact.stat().st_size,
        "architecture": args.arch,
        "compiler": args.compiler,
        "source_commit": args.source_commit,
        "nnue_sha256": sha256(network),
        "nnue_bytes": network.stat().st_size,
        "fixed_signature_nodes": args.fixed_signature,
        "fixed_signature_verified": args.fixed_signature_verified == "true",
        "runtime_identity": runtime_identity(artifact) if args.runtime_engine else None,
    }
    write_json(args.output, payload)
    print(f"Wrote artifact provenance: {args.output}")
    return 0


def assemble(args: argparse.Namespace) -> int:
    dist = args.dist.resolve()
    records = []
    for path in sorted(dist.glob("*.metadata.json")):
        record = json.loads(path.read_text(encoding="utf-8"))
        artifact = dist / record["artifact"]
        if not artifact.is_file():
            raise RuntimeError(f"metadata has no artifact: {record['artifact']}")
        if sha256(artifact) != record["artifact_sha256"]:
            raise RuntimeError(f"artifact hash mismatch: {artifact.name}")
        if record["source_commit"] != args.source_commit:
            raise RuntimeError(f"source mismatch in {path.name}")
        records.append(record)
    if not records:
        raise RuntimeError(f"no artifact metadata found in {dist}")
    network_hashes = {record["nnue_sha256"] for record in records}
    signatures = {record["fixed_signature_nodes"] for record in records}
    if len(network_hashes) != 1 or len(signatures) != 1:
        raise RuntimeError("release artifacts disagree on NNUE or fixed signature")
    manifest = {
        "schema": "coco-release-manifest-v1",
        "tag": args.tag,
        "source_commit": args.source_commit,
        "nnue_sha256": next(iter(network_hashes)),
        "fixed_signature_nodes": next(iter(signatures)),
        "artifacts": sorted(records, key=lambda item: item["artifact"]),
    }
    write_json(args.output, manifest)
    print(f"PASS: verified {len(records)} release artifacts -> {args.output}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    artifact = commands.add_parser("artifact")
    artifact.add_argument("--artifact", type=Path, required=True)
    artifact.add_argument("--arch", required=True)
    artifact.add_argument("--compiler", required=True)
    artifact.add_argument("--source-commit", required=True)
    artifact.add_argument("--network", type=Path, required=True)
    artifact.add_argument("--fixed-signature", type=int, required=True)
    artifact.add_argument("--fixed-signature-verified", choices=("true", "false"), required=True)
    artifact.add_argument("--runtime-engine", action="store_true")
    artifact.add_argument("--output", type=Path, required=True)
    artifact.set_defaults(handler=create_artifact)
    assemble_command = commands.add_parser("assemble")
    assemble_command.add_argument("--dist", type=Path, required=True)
    assemble_command.add_argument("--tag", required=True)
    assemble_command.add_argument("--source-commit", required=True)
    assemble_command.add_argument("--output", type=Path, required=True)
    assemble_command.set_defaults(handler=assemble)
    args = parser.parse_args()
    return args.handler(args)


if __name__ == "__main__":
    raise SystemExit(main())
