import subprocess
import os
import sys
import time
from pathlib import Path

def parse_coco_perft(stdout):
    for line in stdout.splitlines():
        line = line.strip()
        if "Total nodes" in line:
            parts = line.split(":")
            if len(parts) == 2:
                try:
                    return int(parts[1].strip())
                except ValueError:
                    pass
    return None

def main():
    max_test_depth = 3 # Default max depth to keep the test suite running under 5 seconds
    if len(sys.argv) >= 2:
        max_test_depth = int(sys.argv[1])

    project_root = Path(__file__).resolve().parents[1]
    coco_path = Path(os.environ.get("COCO_ENGINE", project_root / "coco-chess.exe"))
    reference_root = Path(os.environ.get("COCO_REFERENCES", project_root.parent / "coco_references"))

    epd_files = [
        ("Custom Debugging Suite", project_root / "testing" / "custom_perft_suite.epd"),
        ("Ethereal Standard Suite", reference_root / "Ethereal" / "src" / "perft" / "standard.epd")
    ]

    if not coco_path.exists():
        print(f"Error: Coco executable not found at {coco_path}")
        return 1

    start_time = time.time()
    all_passed = True
    mismatches = 0

    for suite_name, epd_path in epd_files:
        if not epd_path.exists():
            print(f"Warning: EPD file not found at {epd_path}, skipping.")
            continue

        with epd_path.open("r", encoding="utf-8") as f:
            lines = [line.strip() for line in f if line.strip() and not line.startswith("#")]

        print(f"\n=== Running {suite_name} (max depth {max_test_depth}) ===")
        print(f"Loaded {len(lines)} positions from {epd_path.name}.\n")

        print(f"{'Pos #':<6} | {'Depth':<5} | {'Expected':<12} | {'Coco':<12} | {'Status':<6} | {'FEN'}")
        print("-" * 110)

        for idx, line in enumerate(lines, 1):
            parts = line.split(";")
            fen = parts[0].strip()

            # Test each specified depth up to max_test_depth
            for token in parts[1:]:
                token = token.strip()
                if not token:
                    continue

                # Token format is like "D1 20" or "D4 197281"
                depth_part, nodes_part = token.split()
                depth = int(depth_part[1:])
                expected_nodes = int(nodes_part)

                if depth > max_test_depth:
                    continue

                # Run Coco
                coco_input = f"position fen {fen}\ngo perft {depth}\nquit\n"
                r = subprocess.run([str(coco_path)], input=coco_input, capture_output=True, text=True)
                coco_nodes = parse_coco_perft(r.stdout)

                if coco_nodes == expected_nodes:
                    status = "PASS"
                else:
                    status = "FAIL"
                    all_passed = False
                    mismatches += 1

                # Print brief status
                print(f"#{idx:<5} | D{depth:<4} | {expected_nodes:<12} | {coco_nodes if coco_nodes is not None else 'N/A':<12} | {status:<6} | {fen[:50]}...")

                if status == "FAIL":
                    print(f"  --> ERROR: Node mismatch on FEN: {fen}")
                    print(f"      Stdout was:\n{r.stdout}")

    elapsed = time.time() - start_time
    print("-" * 110)
    print(f"Total Time: {elapsed:.2f} seconds")
    if all_passed:
        print("SUCCESS: All perft positions PASSED!")
        return 0
    else:
        print(f"FAILURE: {mismatches} mismatching perft nodes detected!")
        return 1

if __name__ == "__main__":
    sys.exit(main())
