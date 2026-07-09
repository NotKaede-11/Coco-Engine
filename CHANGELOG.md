# Changelog

All notable changes to the Coco Chess Engine will be documented in this file.

## [1.1.0] - 2026-07-09

This release ships a large batch of search-selectivity, pruning, and tactical-vision upgrades. All changes were verified through the SPRT testing funnel before merge.

### Added
- **Static Exchange Evaluation (SEE):** Added a fast swap-list SEE (`Board::see`) plus an attacker enumeration helper (`get_all_attackers`). Used to prune losing captures in Quiescence Search before they touch the recursive tree.
- **Safe QS Futility Pruning:** Prunes quiet captures in Quiescence Search after the best 2 captures have been searched, using a wide 350 cp margin and excluding promotions.
- **Internal Iterative Reductions (IIR):** Reduces nominal depth by 1 on PV nodes that present a total Transposition Table cache miss at `depth >= 3`, cutting redundant branch exploration.
- **Improving Heuristic:** Tracks whether the static evaluation has improved relative to 2 plies ago and dynamically scales Reverse Futility Pruning margins (`-35` when improving) and Late Move Reductions (`+1` when not improving).
- **Dynamic Stockfish-Style Time Management:** Added best-move stability tracking (stable best move shortens the soft time budget up to -35%; unstable best move extends it up to +40%), a falling-evaluation extension (+35% when the score drops), and an "easy move" single-legal-root early cutoff.
- **Singular Extensions (partial):** Added a conservative, non-recursive singular verification search around the TT move at `depth >= 6`, with a multicut short-circuit when the exclude-search still fails high over beta. TT writes and history updates are suppressed during exclusion searches to avoid pollution.
- **Slider X-Ray King Masking:** Precalculates `checkers`, `pinned`, and per-square `pin_rays` once per node (`LegalityMasks`) via `between_bb`/`line_bb` ray tables, filtering illegal moves before `make_move`. Reduces search nodes while keeping NPS high.
- **Raw TT Entry Probe:** Added `TranspositionTable::probe_entry` exposing stored depth/flag/score/best-move for singular-extension and exclusion-search gating.

### Changed
- **Build Flags:** `build.bat` now compiles with `-march=native -flto` (replacing `-mavx2`) for full host-ISA utilization and link-time optimization. AVX2 intrinsics (`<immintrin.h>`) are now included in the NNUE header.
- **Make/Unmake Refactor:** `make_move` now accepts a `checked` flag so callers that have already validated legality via `LegalityMasks` skip redundant check-escape verification in the hot path.
- **Search Signature:** `alpha_beta` now threads parent evaluations and an `excluded_move` through the recursion to support the improving heuristic and singular/multicut logic.

### Performance
- Bulk perft counting (depth-1 fast path) added to `perft` in `uci.cpp`, boosting perft execution speed.

## [1.0.1] - 2026-07-06

### Added
- **Dynamic UCI Option (`Move Overhead`):** Added option to customize the clock latency buffer dynamically (default: `30` ms).
- **Dynamic UCI Option (`EvalFile`):** Added option to swap and load new neural network weight files (`.nnue`) at runtime without recompilation.
- **NNUE Directory Fallback Loader:** Added automatic fallback path search: if `coco.nnue` is not found in the active working directory, the engine will look next to the running executable.
- **Fail-Fast Engine Startup:** The engine will now exit immediately with code `1` and print an error if `coco.nnue` fails to load, preventing silent "zero-eval" play.
- **Static Compilation build setup:** Embeds all C++ standard runtime dependencies (`libgcc`, `libstdc++`, `winpthreads`) directly inside the executable, removing external DLL requirements.

### Fixed
- **Fullmove Clock Drift:** Fixed a minor state update tracking bug where the fullmove counter drifted on specific move rollbacks.
