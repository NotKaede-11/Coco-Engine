#!/usr/bin/env python3
"""Build and run Coco's C++ correctness fixtures portably."""

from __future__ import annotations

import argparse
import os
import subprocess
from pathlib import Path


CORE_SOURCES = [
    "src/board.cpp",
    "src/movegen.cpp",
    "src/tt.cpp",
    "src/search.cpp",
    "src/evaluate.cpp",
    "src/nnue.cpp",
    "Fathom/src/tbprobe.c",
]

TESTS = [
    "board_state_test",
    "hce_trace_test",
    "mate_distance_test",
    "nmp_guard_test",
    "nnue_symmetry_test",
    "pv_legality_test",
    "qsearch_correctness_test",
    "random_move_test",
    "root_node_accounting_test",
    "search_failsoft_test",
    "see_test",
    "tt_test",
    "uci_score_test",
]


def run(command: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, env=env, check=True, timeout=300)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--build-dir", type=Path, default=Path("scratch/ci-tests"))
    parser.add_argument("--sanitizers", action="store_true")
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    build = (root / args.build_dir).resolve()
    build.mkdir(parents=True, exist_ok=True)

    environment = os.environ.copy()
    compiler_path = Path(args.compiler)
    if compiler_path.parent != Path("."):
        environment["PATH"] = str(compiler_path.resolve().parent) + os.pathsep + environment.get("PATH", "")

    flags = [
        "-std=c++23", "-pthread", "-DL1_SIZE=512", "-DCOCO_TESTING",
        "-IFathom/src", "-O1", "-g",
    ]
    if args.sanitizers:
        flags += ["-fno-omit-frame-pointer", "-fsanitize=address,undefined"]
    link_flags: list[str] = []
    if os.name == "nt" and not args.sanitizers:
        link_flags = ["-static", "-static-libgcc", "-static-libstdc++"]

    objects: list[str] = []
    for index, source in enumerate(CORE_SOURCES):
        obj = build / f"core_{index}.o"
        run([args.compiler, *flags, "-c", source, "-o", str(obj)], root, environment)
        objects.append(str(obj))

    suffix = ".exe" if os.name == "nt" else ""
    if args.sanitizers:
        environment.setdefault("ASAN_OPTIONS", "detect_leaks=1:halt_on_error=1")
        environment.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")

    for test in TESTS:
        executable = build / f"{test}{suffix}"
        run([
            args.compiler, *flags, f"testing/{test}.cpp", *objects,
            "-o", str(executable), *link_flags,
        ], root, environment)
        run([str(executable)], root, environment)

    print(f"PASS: {len(TESTS)} C++ fixture executables")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
