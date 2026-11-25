"""Verify the declared legacy-flat or Ravager-v3 network size and identity."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("network", type=Path)
    parser.add_argument("--l1-size", type=int, default=512)
    parser.add_argument("--format", choices=['flat', 'ravager-v3'], default='flat')
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--sha256")
    args = parser.parse_args()

    if not args.network.is_file():
        parser.error(f"network not found: {args.network}")
    if args.l1_size <= 0:
        parser.error("L1 size must be positive")

    expected_size = 89315360 if args.format == 'ravager-v3' else (
        768 * args.l1_size + args.l1_size + 2 * args.l1_size
    ) * 2 + 4
    payload = args.network.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    print(
        f"network={args.network.resolve()} bytes={len(payload)} "
        f"expected={expected_size} sha256={digest}"
    )
    if len(payload) != expected_size:
        print("FAIL: network size does not match Coco raw NNUE ABI")
        return 1

    expected_digest = args.sha256.lower() if args.sha256 else None
    if args.reference:
        if not args.reference.is_file():
            parser.error(f"reference not found: {args.reference}")
        reference_payload = args.reference.read_bytes()
        reference_digest = hashlib.sha256(reference_payload).hexdigest()
        print(
            f"reference={args.reference.resolve()} bytes={len(reference_payload)} "
            f"sha256={reference_digest}"
        )
        if payload != reference_payload:
            print("FAIL: network differs from reference")
            return 1
        expected_digest = reference_digest

    if expected_digest and digest != expected_digest:
        print(f"FAIL: expected sha256={expected_digest}")
        return 1
    print("PASS: NNUE ABI and hash")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
