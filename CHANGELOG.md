# Changelog

All notable changes to the Coco Chess Engine will be documented in this file.

## [Pre-release]

This development pre-release rebuilds Coco's correctness foundation and expands its search, move generation, evaluation tooling, datagen, protocol support, and release validation. It is published for early testing and feedback before the next stable release.

### Added

- **Stateful Search Infrastructure:** Added staged move picking, dedicated capture/quiet/evasion generation, restored quiescence/PVS/LMR guards, MultiPV, Ponder, complete UCI node and time limits, and corrected Lazy SMP root-node accounting.
- **PEXT Sliding Attacks:** Added an optional BMI2/PEXT backend with tested magic-bitboard fallback and a `Use PEXT` UCI switch.
- **HCE Trace and Tuner:** Added production-aligned classical feature tracing for pawn structure, imbalances, passers, threats, mobility, king safety, endgames, validation splits, checkpoint/resume, and gradient tests. Normal engine play remains NNUE-evaluated.
- **Native Self-Play Datagen:** Added deterministic seeds, private worker TTs, game-state reset, validated TT moves, WDL adjudication, buffered output, exact record targets, and reproducible trainer metadata. With no `--book` argument, openings are generated through engine calculation.
- **Correctness and CI Suites:** Added make/unmake/hash/NNUE accumulator tests, randomized-game stress, TT/fail-soft/NMP/QS/SEE fixtures, noisy-opening regressions, UCI-limit checks, and 149-position perft coverage through D3.
- **Multi-Architecture Releases:** Added Windows POPCNT/AVX2/BMI2/AVX-512, Linux POPCNT/AVX2/BMI2/AVX-512/ARM64/ARM64-dotprod, and macOS Apple Silicon/Intel builds with checksums and license artifacts.
- **Analysis Protocol:** Added complete `searchmoves` filtering, bounded `go mate`, optional WDL output, analysis-mode compatibility, delayed current-move reporting, and tablebase-hit telemetry.
- **Reproducible Artifacts:** Added runtime build/network identity plus manifests for release binaries, self-play datasets, and paired engine matches.

### Changed

- **Compiler Compatibility:** Standardized release, local, and test builds on C++20, matching Coco's actual language-feature requirements and supporting a wider range of Clang and MinGW toolchains.
- **Board and NNUE State:** Replaced per-node accumulator copies with a checked accumulator stack, incremental occupancy updates, and network-safe position rebuilds after `EvalFile` reload.
- **Transposition Table:** Added clustered cache-line storage, generation aging, power-of-two indexing, lockless verification, correct fail-soft bounds, move retention on shallow hits, mate-score conversion, and clear/hashfull support.
- **Evaluation Network Packaging:** The production NNUE is embedded in release binaries; `EvalFile` remains available for deliberate runtime replacement.

### Fixed

- Board-state, en-passant legality, castling, repetition/hash, PV-cycle, TT replacement, and concurrent root-accounting defects found during the ground-up audit.
- Incorrect UCI declaration `SyzygyProbeLimit type bool`; it now uses standard `type check`.
- Incorrect advertised `SEE_Pruning_Depth` default; it now reports the accepted production value of `0`.
- Added standard `debug on` / `debug off` UCI handling.
- Hardened malformed FEN, move, and UCI command handling so rejected positions cannot partially mutate the active board.
- Corrected Syzygy score-band and rule-50 policy handling, including safe repeated tablebase initialization and cleanup.
- Removed unsupported and deferred parameters from the default SPSA profiles.

## [1.4.0] - 2026-07-13

This release marks a massive milestone in Coco's history, delivering an outstanding **+238.25 Elo** strength increase over the v1.3.0 baseline! It introduces highly optimized search structures, a brand-new 100M-position neural network, and deep architectural fixes.

### Added
- **100M-Position Leela Neural Network:** Replaced the default network with a new network trained on 100M positions from Leela Chess Zero (LCZero), providing a massive boost in static evaluation accuracy.
- **Two-Slot Bucket TT:** Upgraded the transposition table replacement scheme to store one depth-preferred entry and one always-replace entry per bucket.
- **On-Demand (Incremental) Move Sorting:** Replaced full selection sorting with an incremental sorting loop, avoiding CPU sorting overhead at nodes that cut off early.
- **History Gravity Decay:** Redesigned quiet and capture history updates to use a smooth mathematical saturation and decay formula (`h += bonus - h*|bonus|/HIST_MAX`), eliminating periodic threshold-division loops.
- **Transposition Table Prefetching:** Integrated compiler prefetching hooks (`__builtin_prefetch`) to hide memory latency during deep tree searches.
- **UCI Options & Contempt:** Added `Contempt` UCI option to avoid early draws, and exposed `SEE_Pruning_Depth` and `LMR_History_Divisor` to allow automated SPSA tuning.

### Changed
- **C++23 `<bit>` Migration:** Migrated low-level bitwise helpers to standard library hardware intrinsics (`std::popcount`, `std::countr_zero`) for maximum CPU instruction-level performance.
- **UCI Version:** Updated the engine version name in the UCI handshake to `Coco v1.4.0`.

### Fixed
- **NNUE Perspective Swap Bug:** Fixed a critical evaluation perspective swapping bug in `evaluate_nnue` where active/passive weight indices were swapped.
- **Board::see Quiet-Move Bug:** Fixed a `Board::see` quiet-move bug where non-captures were incorrectly evaluated with a positive pawn capture value.

---

## [1.3.0] - 2026-07-10

This release introduces a major suite of search, threading, and positional evaluation upgrades. All features have been verified for correctness and stability under extensive search matching and test suites.

### Added
- **Shared Memory Multithreading (Lazy SMP):** Spawns helper threads using independent thread-local search states and board copies while sharing a unified, lockless Transposition Table (TT). Supports up to 1024 threads via the `Threads` UCI option.
- **Syzygy Endgame Tablebase Probing:** Re-integrated Fathom tablebase probing as a native project directory (no submodules). Supports thread-safe WDL probing at non-PV nodes and DTZ probing at the root.
- **On-Demand Enemy Threats Heuristic:** Calculates dynamic enemy attack maps (using a kingless occupancy mask for sliders) to penalize quiet moves that step into heavily defended squares.
- **Contextual Continuation History (CMH + FMH):** Enhances quiet move ordering by indexing history scores based on countermove (ply-1) and follow-up (ply-2) contexts.
- **Capture History Sorting Heuristic:** Sorts tactical captures within their MVV-LVA slots using a dedicated capture history table.
- **NNUE Brain Expansion:** Upgrades the neural network architecture support for larger network layer sizes (512/1024) to improve raw evaluation accuracy.
- **AVX2 Vectorization:** Optimizes hot-path neural network accumulator calculations using vectorized AVX2 intrinsics.
- **Portability & C++23 Migration:** Migrated the project standard to fully stable C++23 and replaced the experimental C++26 `#embed` preprocessor directive with a portable pre-build python hex generator (`scripts/make_nnue_header.py`). Removed Link-Time Optimization (`-flto`) on Windows to resolve binary PE header corruption and standard library compatibility issues.

### Changed
- **UCI Version:** Updated the engine version name in the UCI handshake to `Coco v1.3.0`.

## [1.1.1] - 2026-07-09

### Added
- **Singular Extensions (double-extension):** The singular verification search now grants a +2 ply extension when a TT move is strongly singular (verified well below the singular bound) on non-PV nodes. Cumulative double extensions are capped at 6 to prevent runaway depth inflation.
- **UCI Engine Name:** The engine now identifies itself as `Coco v1.1.0` in UCI handshake.

### Changed
- **Code Formatting:** `src/uci.cpp` reformatted to Allman brace style for consistency.

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
