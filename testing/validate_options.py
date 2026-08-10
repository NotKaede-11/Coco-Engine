import argparse
import subprocess
from pathlib import Path

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("engine", type=Path)
    args = parser.parse_args()
    coco_path = args.engine.resolve()
    if not coco_path.is_file():
        parser.error(f"engine not found: {coco_path}")

    print("Running Coco and checking UCI initialization...")
    project_root = Path(__file__).resolve().parent.parent
    p = subprocess.Popen([str(coco_path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, cwd=project_root)

    # 1. Send uci
    p.stdin.write("uci\n")
    p.stdin.flush()

    uci_output = []
    has_current_identity = False
    has_move_overhead = False
    has_eval_file = False
    has_ponder = False
    has_multipv = False
    has_syzygy_probe_limit = False
    has_syzygy_50_move_rule = False
    has_show_wdl = False
    has_analyse_mode = False
    has_accurate_see_default = False

    while True:
        line = p.stdout.readline().strip()
        uci_output.append(line)
        if line == "id name Coco pre-release":
            has_current_identity = True
        if "option name Move Overhead" in line:
            has_move_overhead = True
        if "option name EvalFile" in line:
            has_eval_file = True
        if "option name Ponder" in line:
            has_ponder = True
        if "option name MultiPV" in line:
            has_multipv = True
        if line == "option name SyzygyProbeLimit type check default true":
            has_syzygy_probe_limit = True
        if line == "option name Syzygy50MoveRule type check default true":
            has_syzygy_50_move_rule = True
        if line == "option name UCI_ShowWDL type check default false":
            has_show_wdl = True
        if line == "option name UCI_AnalyseMode type check default false":
            has_analyse_mode = True
        if line == "option name SEE_Pruning_Depth type spin default 0 min 0 max 20":
            has_accurate_see_default = True
        if line == "uciok":
            break

    if not has_current_identity:
        print("FAIL: UCI identity must be 'Coco pre-release'")
        print("\n".join(uci_output))
        p.terminate()
        return 1
    if not has_move_overhead:
        print("FAIL: 'Move Overhead' option not reported by engine!")
        print("\n".join(uci_output))
        p.terminate()
        return 1
    if not has_eval_file:
        print("FAIL: 'EvalFile' option not reported by engine!")
        print("\n".join(uci_output))
        p.terminate()
        return 1
    if not has_ponder or not has_multipv:
        print("FAIL: Ponder and MultiPV options must both be reported")
        p.terminate()
        return 1
    if not has_syzygy_probe_limit:
        print("FAIL: SyzygyProbeLimit must use the standard UCI 'check' type")
        print("\n".join(uci_output))
        p.terminate()
        return 1
    if not (has_syzygy_50_move_rule and has_show_wdl and has_analyse_mode):
        print("FAIL: Syzygy50MoveRule, UCI_ShowWDL, and UCI_AnalyseMode defaults are missing")
        print("\n".join(uci_output))
        p.terminate()
        return 1
    if not has_accurate_see_default:
        print("FAIL: SEE_Pruning_Depth must advertise the production default of 0")
        print("\n".join(uci_output))
        p.terminate()
        return 1

    print("PASS: Option reporting is correct!")

    # 2. Verify the standard UCI debug command.
    p.stdin.write("debug on\n")
    p.stdin.write("isready\n")
    p.stdin.flush()
    saw_debug = False
    while True:
        line = p.stdout.readline().strip()
        if line == "info string Debug mode on":
            saw_debug = True
        if line == "readyok":
            break
    if not saw_debug:
        print("FAIL: standard 'debug on' command was not acknowledged")
        p.terminate()
        return 1
    p.stdin.write("debug off\n")
    p.stdin.flush()

    # 3. Test setting Move Overhead
    print("Testing setoption for 'Move Overhead'...")
    p.stdin.write("setoption name Move Overhead value 45\n")
    p.stdin.write("setoption name SyzygyProbeLimit value false\n")
    p.stdin.write("setoption name SyzygyProbeLimit value true\n")
    p.stdin.write("setoption name Syzygy50MoveRule value false\n")
    p.stdin.write("setoption name Syzygy50MoveRule value true\n")
    p.stdin.write("setoption name UCI_AnalyseMode value true\n")
    p.stdin.write("setoption name UCI_AnalyseMode value false\n")
    p.stdin.flush()

    # 4. Test setting EvalFile
    print("Testing setoption for 'EvalFile'...")
    p.stdin.write("setoption name EvalFile value coco.nnue\n")
    p.stdin.flush()

    # Read response
    p.stdin.write("isready\n")
    p.stdin.flush()

    has_loaded_msg = False
    while True:
        line = p.stdout.readline().strip()
        print(f"Engine: {line}")
        if "NNUE weights file loaded successfully" in line or "Loaded network" in line or "loaded" in line:
            has_loaded_msg = True
        if line == "readyok":
            break

    p.stdin.write("quit\n")
    p.stdin.flush()
    p.wait()

    if has_loaded_msg:
        print("PASS: Dynamic options and network loading verified successfully!")
        return 0
    else:
        print("FAIL: Engine did not print success message for reloading network!")
        return 1

if __name__ == "__main__":
    raise SystemExit(main())
