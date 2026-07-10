# 🗺️ Master Coco Chess Engine Roadmap

This document outlines the evolutionary milestones for the Coco Chess Engine. It serves as our master checklist and development guide.

---

## 🧪 Testing & Tuning Protocols

To ensure that every code modification or brain update results in a genuine strength increase, we use three distinct testing phases:

### 1. SPSA (Parameter Tuning) — _When to run: After introducing new variables_

- **Purpose:** Automatically optimize parameters (e.g., LMR constants, pruning margins, history scaling).
- **Book to Use:** Must use a balanced, fast-play book from the `books/` folder (such as **`8moves_v3.pgn`** or **`noob_3moves.epd`**). This ensures parameters are tested across diverse, playable middlegame positions without starting-position bias.
- **Command Reference:** Run python tuning scripts linked to `fastchess.exe` (e.g., `tune_30k.py`) for a minimum of 10,000 to 30,000 fast games.
- **Pass Condition:** The SPSA loop converges on optimal, stable values.

### 2. SPRT (Regression & Elo Verification) — _When to run: After completing any item_

- **Purpose:** Prove that a change is a net positive before committing/deploying.
- **Book to Use:** Use a balanced book like **`noob_4moves.epd`** or **`8moves_v3.pgn`** to keep results realistic and avoid rating compression/winrate saturation.
- **Command Reference:** Run `fastchess.exe` with the `-sprt` command using parameters `elo0=0` (no change) and `elo1=5` (5 Elo gain), testing `New_Coco` vs. `Old_Coco`.
- **Pass Condition:** The match terminates early with a **Green Pass** (statistically significant Elo gain).

### 3. PROGTEST (Progress Assessment) — _When to run: After major milestones (Tiers)_

- **Purpose:** Determine Coco's absolute Elo progress against a fixed suite of reference engines.
- **Book & Opponent Strategy:**
  - **Book:** Use **`8moves_v3.pgn`** or **`noob_4moves.epd`** to ensure a balanced, standard opening distribution. Never use unbalanced books (UHO) at short time controls.
  - **Opponent Selection:** Run the test as a gauntlet against **two successive versions** of a reference engine (e.g. Stash v14 and Stash v15) to bracket Coco's exact position. Avoid huge 3-digit Elo gaps which distort statistics.
- **Command Reference:** A gauntlet tournament in `fastchess.exe` against a pool of baseline engines at fixed time controls.
- **Pass Condition:** Coco achieves a positive score and climbs the internal rating database.

### 4. PERFT (Move Generator Debugging) — _When to run: After any change to board or movegen_

- **Purpose:** Ensure the move generator is 100% bug-free. Even a 1-node error at depth 5/6 indicates a critical logic bug (e.g., illegal castling, en passant, or king check leaks).
- **Command Reference:** Run the automated perft suite using `python testing/run_perft_suite.py <depth>` which tests **149 positions** (21 custom CPW/Maverick/Sedlak debugging positions + 128 standard Ethereal positions) **alongside Stockfish** to verify node count equivalence.
- **Pass Condition:** Total calculated leaf nodes match reference Stockfish counts down to the single node at every depth.

---

## ⏱️ Time Control (TC) & Hash Standards

To verify that Coco's heuristics scale cleanly under different time constraints and memory allocations (preventing short-term fixes from degrading long-term scaling), we use three standard test configurations:

### 1. STC (Short Time Control / Blitz)

- **Time Control:** `8+0.08s` (for normal search logic/heuristics) or `2+0.02s` (specifically for speedups / NPS increase patches)
- **Hash Size:** `16MB` or `32MB`
- **Purpose:** Standard SPSA tuning runs, search logic verification, and rapid regression checking for NPS speedups.

### 2. LTC (Long Time Control / Rapid)

- **Time Control:** `40+0.4s` (40 seconds base + 400ms increment)
- **Hash Size:** `64MB`
- **Purpose:** Verification that search and pruning heuristics scale positively at deeper search plies (avoiding "LTC regression").

### 3. VLTC (Very Long Time Control / Classical)

- **Time Control:** `120+1.2s` (or `240+2.4s` for final benchmarks)
- **Hash Size:** `128MB` or `256MB`
- **Purpose:** High-depth verification of major scaling features like multithreading (Lazy SMP) and dual-brain neural net resolution under classical tournament time budgets.

## 🔄 The SPRT & SPSA Testing Funnel

To optimize testing efficiency on consumer hardware, we follow a strict multi-tier testing pipeline. We never run LTC or VLTC tests for unverified patches:

1. **Parameter Tuning (SPSA):**
   - **Rule:** SPSA is **always run at STC** (`8+0.08s`) to ensure we can play the 10k–30k games required to converge on stable parameters.
   - **Verification:** Once SPSA finishes, the final optimized parameters are verified in a single **LTC SPRT** match.

2. **NPS Speedups (Code Optimizations):**
   - **Pipeline:** Test at **STC (`2+0.02s`)** ➡️ If Green Pass, merge immediately.
   - **Rule:** Do not waste time running LTC/VLTC tests. Pure speedups scale mathematically perfectly to all time controls.

3. **Search Logic & Pruning Tweaks (LMR, Pruning, Reductions):**
   - **Pipeline:** Test at **STC (`8+0.08s`)** ➡️ If Green Pass, test at **LTC (`40+0.4s`)** ➡️ If Pass, merge.
   - **Rule:** If a search tweak fails at STC, discard it immediately. If it passes STC, it _must_ pass LTC to verify it does not suffer from "LTC regression" at high depths.

4. **Major Scaling & Architecture Upgrades (Lazy SMP, Neural Net L1 scale, CorrHist):**
   - **Pipeline:** Test at **STC (`8+0.08s`)** ➡️ Test at **LTC (`40+0.4s`)** ➡️ Verify at **VLTC (`120+1.2s`)** ➡️ Merge.
   - **Rule:** Only use VLTC to verify that complex thread-scaling or deep historical evaluation adjustments scale cleanly under classical tournament limits.

---

## 🧠 NNUE Brain Upgrade & Datagen Schedule

To keep the neural network aligned with our search changes, we will rebuild and upgrade Coco's brain at the following milestones:

### 🚀 Milestone 1: The 100M Baseline (Postponed - End of Tier 6 / After Threat-Input Network)

- **Datagen Setup:** 100M positions (80/20 split: 80M Coco + 20M Leela).
- **Stockfish Evaluation Depth:** Depth 8 evaluation.
- **Execution Strategy:**
  - > [!IMPORTANT]
  - > **Core Search & Threat-Input Network First:** As aligned, all NNUE datagen and training is postponed until the very end of Tier 6, specifically **after** we have completed the **Threat-Input Neural Network Architectures** task. This ensures our final net has both optimal search-aligned paths and advanced threat-map inputs.
- **Leela Dataset Source:** `test80-2024-06-jun-2tb7p.min-v2.v6.binpack.zst` (7.9 GB v6-filtered binpack from `linrock/test80-2024` Hugging Face - placed locally in `bullet/data/`).
- **Brain Target:** Rebuild the single `L1 = 256` network.
- **Goal:** Re-train a search-aligned baseline brain after all search core features and network architecture modifications are completed.

### 🚀 Milestone 2: The Big Brain Scale-up (Post-Tier 6 / Production Net)

- **Datagen:** 250M positions (80/20 split: 200M Coco + 50M Leela).
- **Brain Target:** Upgrade network architecture size to **`L1 = 512`** or **`L1 = 1024`**.
- **Goal:** Scale up the brain capacity once the search core is fully finalized.

### 🚀 Milestone 3: The Super-Engine Dual Brain (Post-Tier 6 / Final Goal)

- **Datagen:** 100M (Tactical Small Net) + 500M (Strategic Bucketed Big Net) — **Maintaining the 80/20 Coco/Leela split** to ensure Coco retains Leela's deep positional and endgame wisdom even at peak rating levels.
- **Brain Target:** Dual NNUE routing (Piece-Count & King Buckets).
- **Goal:** Peak performance setup (3400+ Elo).

---

## 📈 SPSA Tuning Notice

- > [!NOTE]
- > **Postponed Tuning:** All `[SPSA]` tuning tasks are postponed to Tier 6. Running SPSA locally on a single machine is extremely slow and blocks development. Once our OpenBench server and parallel testing workers are fully set up in Tier 6, we will run automated SPSA tuning rounds to optimize all search heuristics (margins, LMR constants, etc.) in parallel.

---

## 🗺️ Master Checklist

### Tier 1: Foundations and Core Infrastructure (Elo: 0 to 1200)

- `[X]` **Bitboard Board Representation:** Mapping the 64-square grid onto 64-bit unsigned integers (`uint64_t`).
- `[X]` **Attack Generation (Leap and Sliding):** Precalculating step arrays for Knights/Kings and using Magic Bitboards for sliding Rooks, Bishops, and Queens.
- `[X]` **Pseudo-Legal Move Generator:** High-velocity loops calculating every possible physical leap and slide across the board.
- `[X]` **Illegal Move Filtering:** Dynamic check validation checking if a move leaves the friendly king exposed; handles check escapes.
- `[X]` **Perft Verification Suite `[PROGTEST]`:** Passing brute-force combinatorial node counts across standard startpos and deep tactical benchmarks like Kiwipete.

---

### Tier 2: The Neural Brain and Basic Search (Elo: 1200 to 1800)

- `[X]` **Baseline Alpha-Beta Loop:** The primary Negamax recursive framework managing alpha and beta scoring window boundaries.
- `[X]` **Transposition Table (TT):** A flat contiguous hash map tracking Zobrist keys to cache depth, flags, and best moves to prevent redundant node analysis.
- `[X]` **Quiescence Search:** A selective sub-tree loop processing only captures and promotions to eliminate the horizon effect and establish a clean standing pat baseline.
- `[X]` **Move Ordering (Phase 2 Baseline):** Sorting key paths using the TT move first, followed by captures ranked via MVV-LVA tables.
- `[X]` **Dataset Extraction Pipeline:** Streaming millions of quiet positions out of raw Grandmaster PGN zip archives while stripping out tactical noise and checks.
- `[X]` **Perspective-Relative NNUE Training `[PROGTEST]`:** Mapping feature layers (768 inputs) relative to the side to move and quantizing PyTorch floats into structured C++ integer weights.
- `[X]` **Incremental Accumulator Optimization `[SPRT]`:** Upgrading evaluation to constant O(1) time by adjusting the hidden layer state during `make_move` and `unmake_move` loops rather than re-scanning the entire board.
- `[X]` **Evaluation Symmetry Unit Test `[PERFT]`:** Implementing an automated test that mirrors the board state, swaps colors, and verifies the NNUE evaluation returns the exact same score.
- `[X]` **Random-Move Game Simulation Stress-Test `[PERFT]`:** Simulating thousands of rapid random-move games to catch edge-case crashes, illegal move leaks, or repetition/draw tracking failures.

---

### Tier 3: Tree Reduction and Advanced Ordering (Elo: 1800 to 2400)

- **Tuning & Elo Reference Bracket:** Stash v14 (2054 Elo) to Stash v18 (2380 Elo)

* `[X]` **Null Move Pruning (NMP) `[SPRT]`:** Simulating a pass move to verify if the defensive position is secure enough to allow massive depth skips.
* `[X]` **Late Move Reductions (LMR) `[SPSA]` `[SPRT]`:** Scaling down the depth allocation of quiet moves that appear late in highly sorted move lists.
* `[X]` **Killer Move Heuristic `[SPRT]`:** Storing the top two non-capture moves per ply that cause immediate beta cutoffs to fast-track quiet sorting.
* `[X]` **History Heuristic Table `[SPRT]`:** Tracking a global table to prioritize quiet positional choices across separate sub-trees based on past cutoff success.
* `[X]` **Principal Variation Search (PVS) `[SPRT]`:** Forcing zero-width null windows (`[-alpha - 1, -alpha]`) on all sibling moves to trigger rapid failure cutoffs.
* `[X]` **LMR Logarithmic Table Adjustment `[SPSA]` `[SPRT]`:** Swapping out raw integer divisions for smooth logarithmic curve lookup tables.
* `[X]` **History Heuristic Maluses `[SPSA]` `[SPRT]`:** Penalizing the history tables of quiet moves that failed to create cutoffs before the winning move was found.
* `[X]` **PV-Node LMR Exemption `[SPRT]`:** Guarding the primary principal line from depth reductions to secure tactical accuracy.
* `[X]` **Aspiration Windows `[SPSA]` `[SPRT]`:** Narrowing initial root depth search windows around the previous ply score to minimize total root node exploration.
* `[X]` **Static Null Move Pruning (RFP) `[SPSA]` `[SPRT]`:** Pruning near-leaf positions immediately if the static evaluation minus a safe margin remains completely above beta.
* `[X]` **Draw Shield Gating `[SPRT]`:** Early return tracking for 50-move thresholds and threefold repetition loops within child nodes (`ply > 0`) to enforce tree safety.
* `[X]` **Elastic Clock Boundaries `[SPRT]`:** Splitting time budget into distinct Hard and Soft boundaries, managed by Node-Count thresholds.
* `[X]` **UCI GUI Silence Protocol:** Exposing explicit boilerplate configurations for `option name Hash` and `option name Threads` to quiet validation warnings in third-party automated tourney managers.
* `[X]` **Deterministic Engine `bench` Command `[PROGTEST]`:** Implementing a CLI `bench` command that runs a search on a fixed array of standard tactical positions and yields a deterministic node count to catch search refactor bugs.
* `[X]` **Dynamic UCI Options (Move Overhead & EvalFile) `[SPRT]`:** Expose custom UCI options to let users configure the neural network path (`EvalFile` / `File`) and network safety buffer (`Move Overhead`) dynamically at runtime. Completed and verified.

---

### Tier 4: Selective Deepening and Advanced Pruning (Elo: 2400 to 2800)

- **Tuning & Elo Reference Bracket:** Stash v20 (2509 Elo) to Stash v22 (2770 Elo)

* `[X]` **Branchless Move Generation (Capture/Quiet Bitboard Splitting) `[SPRT]`:** Splitting move bitboards into captures and quiets using bitwise operations before loops to eliminate conditional branches inside generator hot paths.
* `[X]` **Bulk Perft Counting `[PERFT]`:** Bypassing standard recursive move search when `depth == 1` to directly count legal leaf nodes, boosting perft execution speed.
* `[X]` **Slider X-Ray King Masking (Movegen Refinement Pass) `[PERFT]` `[SPRT]`:** Precalculating checking/pinning rays once per search node to filter illegal moves before making them. Achieved up to -25% search node reduction while maintaining peak NPS, passing SPRT with +48 ELO!
* `[X]` **Razoring `[SPSA]` `[SPRT]`:** Depth-1 validation exit gate that drops hopeless quiet nodes directly to Quiescence Search to avoid movegen overhead.
* `[X]` **Futility Pruning `[SPSA]` `[SPRT]`:** Dropping remaining quiet move lists at low depths if the static evaluation plus a safety cushion cannot reach alpha.
* `[X]` **Move Count Based Pruning (LMP) `[SPSA]` `[SPRT]`:** Dropping calculations entirely after a specific threshold of quiet moves has been generated and failed at a node.
* `[X]` **Static Exchange Evaluation (SEE) `[SPRT]`:** Evaluating the material balance of capture sequences on a single square using a fast point system to prune obviously losing tactical trades before they touch search. _(References: [Leorik/Leorik.Test/see.epd](Leorik/Leorik.Test/see.epd), [Ethereal/src/board.c](Ethereal/src/board.c), [weiss/src/search.c](weiss/src/search.c))_
* `[X]` **Safe QS Futility Pruning `[SPRT]`:** Pruning quiet captures in Quiescence Search with safety guards (only after searching the best 2 captures, using a wide 350 cp margin, and excluding promotions). Passed SPRT with +61.71 ELO!
* `[X]` **Internal Iterative Reductions (IIR) `[SPRT]`:** Reducing nominal depth allocations on PV nodes that present a total Transposition Table cache miss at depth >= 3. Achieved massive branch reduction, passing SPRT with +99 ELO!
* `[X]` **Improving Heuristic Logic `[SPSA]` `[SPRT]`:** Dynamically scaling RFP margins and LMR reductions based on whether our position has improved relative to 2 plies ago. Passed SPRT with +57.75 ELO!
* `[X]` **Dynamic Stockfish-Style Time Management `[SPRT]`:** Implementing "Easy Move" early cutoff for single legal moves, best-move stability tracking, and falling evaluation search extensions. Passed SPRT with +38.81 ELO!
* `[-]` **Singular Extensions `[SPSA]` `[SPRT]`:** Running shallow null-window checks on dominant TT moves; if no other move can compete, extending the main search path by +1 ply to see through deep tactical nets.

---

### Tier 5: Hardware Concurrency and Perfect Knowledge (Elo: 2800 to 3200)

- **Tuning & Elo Reference Bracket:** Stash v24 (2880 Elo) to Stash v30 (3162 Elo)

* `[X]` **Native C++ Self-Play Data Generator:** High-performance, in-memory multi-threaded engine subsystem running heap-allocated (`std::make_unique<Board>`) game loops to bypass Python pipeline context switching.
* `[X]` **Internal Iterative Deepening (IID) `[SPRT]`:** Spawning miniature tactical searches on nodes that lack a valid TT move to establish baseline sorting layout on cache misses.
* `[X]` **SIMD Vectorization (AVX2 Intrinsics) `[PERFT]` `[PROGTEST]`:** Rewriting the inner math calculation loops of the NNUE forward pass using assembly register intrinsics to compute 16 operations per clock cycle. _(References: [nnue-tools/](nnue-tools/), [stockfish/src/nnue/layers/](stockfish/src/nnue/layers/))_
* `[X]` **NNUE Brain Expansion (L1 Size Upgrade to 512/1024) `[PROGTEST]`:** Upgrading the hidden layer size of the network to double or quadruple its capacity to fully exploit the 100M dataset, launched alongside AVX2 Vectorization to offset the speed cost.
* `[X]` **Shared Memory Multithreading (Lazy SMP) `[PROGTEST]`:** Spanning search loops across multiple native CPU threads that read and write concurrently to the same global Transposition Table. _(References: [stockfish/src/thread.cpp](stockfish/src/thread.cpp), [Ethereal/src/search.c](Ethereal/src/search.c))_
* `[X]` **On-Demand Enemy Threats Bitboard `[SPRT]`:** Integrating quick, non-incremental attack maps generated from scratch to actively penalize quiet moves stepping into heavily defended enemy fire zones. _(Reference: [meowshatranj/src/position.rs](meowshatranj/src/position.rs#L550) using kingless_occ)_
* `[X]` **Contextual Continuation History (CMH + FMH) `[SPSA]` `[SPRT]`:** Linking history table indexing to structural pairs of moves (Countermove and Follow-up configurations) to enhance deep quiet path sorting stability.
* `[X]` **Capture History Sorting Heuristic `[SPSA]` `[SPRT]`:** Isolating non-winning and losing tactical captures into an independent heuristic table to refine Quiescence and PVS sorting accuracy.
* `[X]` **Syzygy Endgame Tablebases `[PROGTEST]`**: Connecting direct file stream hooks to `.rtbw` and `.rtbz` data tables to instantly pull exact mathematical win, loss, or draw vectors for 3, 4, or 5 piece endgames. _(References: [Fathom/](Fathom/))_

---

### Tier 6: The Super-Engine Era (Elo: 3200 to 3700+)

- **Tuning & Elo Reference Bracket:** Stash v33 (3282 Elo) to Stash v37 (3431 Elo)

* `[ ]` **SEE Capture Pruning Gating `[SPRT]`:** Running full SEE analysis inside PVS and Quiescence loops to prune structurally deficient capture sequences before they touch recursive search lines.
* `[ ]` **History-Based Alpha Pruning `[SPSA]` `[SPRT]`:** Dropping low-probability quiet moves entirely from the branch if their global history heuristic weight sits beneath a deep negative threshold.
* `[ ]` **Multicut Search Evaluation `[SPRT]`:** Probing null-window sub-trees across sibling moves; if multiple options independently trigger beta cutoffs, returning beta immediately without finishing the move loop.
* `[ ]` **Correction History Tables (CorrHist) `[SPSA]` `[SPRT]`:** Tracking and averaging historical variance between deep search returns and raw static evaluations; this allows the engine to naturally identify deep positional blockades, fortresses, and complex repetition traps.
* `[ ]` **Threat-Input Neural Network Architectures `[PROGTEST]`:** Upgrading the feature mapping layers of your network to explicitly track active attacking and defending lines across squares, leveraging a King Input Bucketing scheme (Mixture of Experts) across localized king coordinates.
* `[ ]` **Multi-Network Architectures (Dual/Quad Networks) `[PROGTEST]`:** Shipping your engine with multiple networks (such as a small fast network for shallow high-speed nodes and a large complex network for deep analytical evaluation nodes routed via Material Piece-Count Buckets).
* `[ ]` **Distributed Testing Framework (OpenBench Server Setup) `[PROGTEST]`:** Setting up an OpenBench server on PythonAnywhere linked to your GitHub repo and configuring workers to automate remote test execution.

---

## 🔎 Cross-Engine Reference Notes for Remaining Items

These notes are logic-only references from the other engine folders in this workspace. They are meant to guide implementation choices, not to copy code.

### Tier 4

- `[-]` **Singular Extensions:** Stockfish treats singular extension as a narrow verification search around the TT move, with depth scaled by how much the best move stands out. Keep Coco's version equally conservative and non-recursive so it only promotes truly isolated tactical moves.

### Tier 5

- `[X]` **SIMD Vectorization (AVX2 Intrinsics):** Leorik's AVX2 path shows the safe pattern is to isolate vectorization to the hottest eval/math loops, while Stockfish's NNUE feature split (`Full_Threats`) suggests keeping the vectorized fast path feature-specific instead of turning the whole eval into one monolith.
- `[X]` **NNUE Brain Expansion (L1 Size Upgrade to 512/1024):** Leorik 3.1 pairs a larger network with correction history and dynamic late-move reductions, which is a useful reminder that wider nets usually need search tuning and stability checks, not just a bigger model file.
- `[X]` **Shared Memory Multithreading (Lazy SMP):** Leorik and Stockfish both rely on lockless shared search data while keeping thread-local search state independent. Stockfish's shared-history-per-NUMA-node idea is the main takeaway here: share the data that helps ordering, but avoid unnecessary cross-node traffic.
- `[X]` **On-Demand Enemy Threats Bitboard:** Ethereal and Weiss both keep threat information cached for eval and legality, while meowshatranj recomputes threats from a kingless occupancy mask for sliders. The useful concept is one cheap cached threat board plus a clear recompute path for king-safety-sensitive positions.
- `[X]` **Contextual Continuation History (CMH + FMH):** Leorik's counter/follow-up move ordering and Stockfish's continuation history types both point to the same structure: keep move-pair history separate from killer history so the table captures context instead of just raw move identity.
- `[X]` **Capture History Sorting Heuristic:** Leorik explicitly excludes captures from killer/counter/follow-up tracking, which supports a separate capture-history table. That keeps quiet-move learning clean and lets tactical move ordering evolve independently.
- `[X]` **Syzygy Endgame Tablebases:** Stockfish, Fathom, and Weiss all agree on the key integration shape: a thin tablebase adapter with explicit WDL/DTZ probing, rule-50 handling, and root-move preselection. The roadmap should keep this as an interface problem, not as a search rewrite.

### Tier 6

- `[ ]` **SEE Capture Pruning Gating:** Leorik uses SEE to skip bad captures in quiescence and to demote poor moves in the main search. The practical lesson is to gate SEE pruning late enough that TT/PV context can still protect tactical resources.
- `[ ]` **History-Based Alpha Pruning:** This should be tied to corrected evaluation or continuation history, not a single raw history number. Otherwise the pruning threshold can drift when the eval changes.
- `[ ]` **Multicut Search Evaluation:** Stockfish's singular-extension logic already pairs singularity checks with multi-cut-style reasoning. Keep Coco's multicut trigger narrow and depth-limited so it remains a tactical shortcut, not a broad pruning policy.
- `[ ]` **Correction History Tables (CorrHist):** Stockfish splits correction history into pawn, minor, non-pawn, piece-to, and continuation buckets, and even shares them carefully across NUMA nodes. The key insight is to correct different eval contexts separately rather than collapsing everything into one global correction scalar.
- `[ ]` **Threat-Input Neural Network Architectures:** Stockfish's `Full_Threats` NNUE feature is the strongest local hint here: model threats as dedicated input structure, then keep the feature layout explainable so routing by king bucket stays stable.
- `[ ]` **Multi-Network Architectures (Dual/Quad Networks):** The roadmap should treat this as a routing problem first and a capacity problem second. Use a small fast network for shallow/high-speed nodes and reserve the larger net for positions whose complexity justifies the extra latency.
- `[ ]` **Distributed Testing Framework (OpenBench Server Setup):** Weiss and Stockfish both reinforce the need for stable, automated play/testing infrastructure with explicit UCI options and balanced book usage. The implementation target should preserve the same disciplined testing funnel already described above, but run it on distributed workers.

---

## 🔬 Deep-Scan Cross-Engine Insights (Logic Only — Do Not Copy Code)

These are additional concrete findings from a deeper scan of every engine folder in this workspace. They supplement the notes above with parameters, file references, and structural decisions to guide Coco's implementation. Logic only — no code is copied.

### Tier 4 — Singular Extensions `[-]`

- **Verification shape (Stockfish `search.cpp:1119-1181`):** singular is decided by a single *excluded re-search*. The TT move is stored on the search stack (`ss->excludedMove`) and skipped in the move loop. Verify depth = `newDepth / 2`, not `(depth-1)/2`. Entry gate: `depth >= 6 + ss->ttPv` (PV nodes need 1 less), `ttData.depth >= depth - 3`, `ttData.bound & BOUND_LOWER`, and a `!is_shuffling(move)` guard (suppresses singular on ping-pong king moves when rule50<10 / pliesFromNull<=6 / ply<20).
- **Three-way decision (Stockfish):** singular, multicut, and negative extension are *one* decision block keyed on the exclude-search result vs three bounds (`singularBeta`, `beta`, `singularBeta - doubleMargin/tripleMargin`). `singularBeta = ttValue - (53 + 75*(ttPv && !PvNode))*depth/60`. Positive extension up to **+3** (`+1` singular, `+1` for double margin, `+1` for triple margin). Margins are *correction-history-adjusted* (`corrValAdj = |correctionValue|/230673`).
- **Negative extension (the dual):** when not singular and no multicut, the ttMove is *reduced* (`-3` if `ttValue >= beta`; `-2` if cutNode). This is the "reward alternatives, demote the non-singular ttMove" principle.
- **Ethereal variant (`search.c:1022-1057`):** exclusion via parent-ply `NodeState.excluded`; verify at `(depth-1)/2` with `[rBeta-1, rBeta]`, `rBeta = max(ttValue - depth, -MATE)`. Double-extension cap **6** (`dextensions <= 6`). Picker stage reset to `STAGE_TABLE+1` to reuse the initialized picker — no duplicate move list.
- **Weiss variant (`search.c:489-514`):** verify at `depth/2`, `singularBeta = ttScore - depth*(2 - pvNode)` (PV nodes get a *smaller* margin). Double-extension cap **5**. Multicut fires by a direct `return singularBeta` (cleanest variant).
- **Double-extension cap:** every implementation caps double extensions (5–6) to prevent runaway depth inflation — Coco must cap too.

### Tier 5

- **SIMD/AVX2 (Stockfish `nnue/simd.h`, `nnue/layers/`):**
  - Quant types: `BiasType=int16`, `WeightType=int16`, threat weights `int8`, `PSQTWeightType=int32`, accumulator output `uint8`. `OutputScale=16`, `WeightScaleBits=6`, `SimdWidth=32` (AVX2), `NumRegistersSIMD=12`.
  - ClippedReLU uses `_mm256_packus_epi32` (free lower clip to 0) + `srli 6` + `_mm256_packs_epi16` (upper clip to 127) + a `permutevar8x32` to undo lane reorder. SqrClippedReLU uses `_mm256_mulhi_epi16` (keeps sign) with an extra `>>7` baked into the trainer.
  - Sparse-input affine (`affine_transform_sparse_input.h`): `find_nnz` uses a 256-entry LUT over an 8-bit nonzero mask (AVX2) to gather only nonzero columns; under VNNI, accumulators split into **3 dependency chains** to hide dot-product latency.
  - Weight layout is *scrambled* (`get_weight_index_scrambled`) for sequential SIMD access; weights stored LEB128-compressed.
  - **Ethereal (`nnue/archs/avx2.h`, `nnue.c:160-248`):** 8 accumulator regs, `maddubs`+`madd`, weight shuffle interleaves 256-bit chunks so the HalfKP split is free at runtime; **delayed dequantization** multiplies biases by `1<<SHIFT_L1` to eliminate runtime `srai`.
  - **Leorik (`NeuralNetEval.cs:233-268`):** SCReLU via `Vector256.Max(Min(accu,255),0)` then `(a*w)*a` (not `a*a*w`) to avoid int16 overflow, then `MultiplyAddAdjacent`.
  - **Bullet inference reference (`examples/progression/simple.rs:113-211`):** accumulator rows as `#[repr(C, align(64))] [i16; HIDDEN]`; dequant path `Σ screlu(acc)*out_w; /=QA; +=bias; *=SCALE; /=QA*QB`. Useful as the target contract Coco's C++ must match.

- **NNUE Brain Expansion (Stockfish `nnue_architecture.h:43-71`):**
  - Big net `L1=1024`, `L2=15`, `L3=32`; small net `L1=128`, same L2/L3. Stack: `fc_0` sparse affine → SqrClippedReLU + ClippedReLU (doubled feature) → `fc_1` → ClippedReLU → `fc_2` → output, with a forward skip from `fc_0_out[L2]` scaled by `600*OutputScale/(127*(1<<WeightScaleBits))`.
  - 8 layer-stacks (independent weight sets per file), bucketed by `(pieceCount-1)/4`. PSQT accumulator is bucketed too.
  - **Ethereal (`nnue/types.h:35-42`):** `L1=1536` (=2×768 both perspectives), `L2=8`, `L3=32`. Mixed quant: L0 int16, L1 int8+int32 bias→float, L2/L3 float+FMA. Output is *phased* MG/EG (`MakeScore`) — unusual among NNUE engines.
  - **Leorik (`Network.cs`):** ships a 1HL→768HL scaling study; deployed `L1=640`, single hidden layer with SCReLU, `QA=255/QB=64/SCALE=400`. 5 input buckets (king-square) + 8 output buckets (material). FRC/Chess960 supported. King-bucket change forces a rebuild of that perspective.
  - **Bullet progression advice (`docs/1-basics.md:99-112`):** start with `(768→1536)x2→1×8` (no input buckets, single hidden layer); >1 hidden layer "is non-trivial to get them to not lose lots of elo due to the speed hit." Supports Coco's "wider nets need search tuning + stability checks, not just a bigger file" note.

- **Shared Memory Multithreading (Lazy SMP):**
  - **Stockfish:** per-`Thread` `Search::Worker`; main thread holds the `SearchManager`, others a `NullSearchManager`. One global *racy* TT shared by all (synchronization costs Elo). **NUMA-replicated networks** (`LazyNumaReplicated`): instance 0 loaded eagerly, other NUMA nodes created lazily on first access *on the target node*. **Per-NUMA-node shared histories** (`SharedHistories` = `UnifiedCorrectionHistory` + `PawnHistory`) sized to `next_power_of_two(count)` threads on that node to avoid cross-node traffic.
  - **Depth staggering:** aspiration `delta = 5 + threadIdx%8 + |meanSquaredScore|/9000`; `increaseDepth` flag toggles `searchAgainCounter` so under time pressure some iterations repeat at reduced depth (`adjustedDepth = max(1, rootDepth - failedHighCnt - 3*(searchAgainCounter+1)/4)`).
  - **Best-thread voting:** `(score - minScore + 14) * completedDepth`, with special handling for proven win/loss (shortest mate).
  - **Ethereal (`search.c:213-225`):** pthreads, helpers run `iterativeDeepening` independently; `select_from_threads` picks best by depth then score with mate-distance awareness — the most robust worker selection of the three. Shared TT 3-entry bucket (32-byte aligned). NUMA bind only when `nthreads > 8`. Per-thread `NNUEEvaluator` + Finny table.
  - **Weiss (`threads.c:125-129`):** pthreads, helpers iterate to `MAX_PLY` regardless of main depth. **Parallel TT clear** divides the table into 2 MB slices across threads; **Linux huge pages** via `madvise(MADV_HUGEPAGE)` + 2 MB alignment. Stack `ss[128]` with `SS_OFFSET=10`; negative-offset entries pre-initialized to dummy continuation/contCorr sentinels.
  - **Leorik (`ParallelSearch.cs`):** `Parallel.For` over workers, shared static TT (2-slot bucket, XOR `Key^Data`, undercut replacement `entry.Depth+entry.Age > depth+1+_age`). Worker selection is the weakest (last-finished) — a cautionary example.
  - **Large pages:** both Stockfish and Weiss use large-page allocation for workers/big histories to cut TLB pressure.

- **On-Demand Enemy Threats Bitboard:**
  - **meowshatranj (`position.rs:529-580`):** the cleanest reference. Cache `checkers`, `pinned`, `threats` on the `Position` together; recompute per node via `update_attacks` (called from `apply_move`/`apply_nullmove`/`regen`). **`kingless_occ = occ ^ king_sq.bb()`** — the side-to-move's king is removed from occupancy *only for slider (rook) threat generation*, so threats "see through" the king square. Pawns via `pawn_attacks_setwise` (single shift-OR, no loop); knights/king via lookup tables. Pin detection = "potential attacker + ray-between": empty → checker, exactly one → pinned.
  - **Ethereal (`board.h:28`, `attacks.c:235-253`):** `Board.threats` = all squares attacked by side-not-to-move, recomputed every `apply()` and saved/restored via `Undo.threats`. Consumed by history: `threat_from`/`threat_to` booleans *slice the butterfly-history and capture-history tables into 4 sub-tables each* — threats augment move-ordering granularity cheaply (unique to Ethereal, low-risk Elo gain).
  - **Weiss:** no persistent threat board; attack maps built per-eval in `EvalInfo` (`attackedBy[COLOR][TYPE]`, `mobilityArea`, `kingZone`, `attackPower`, `attackCount`), only the pawn part cached.
  - **Concept for Coco:** one cheap cached threat board (meowshatranj shape) + a clear recompute path using `kingless_occ` for king-safety-sensitive positions; optionally adopt Ethereal's threat-augmented history slicing.

- **Contextual Continuation History (CMH + FMH):**
  - **Stockfish (`history.h:147-150`):** `ContinuationHistory = [PIECE_NB][SQUARE_NB]` of `PieceToHistory`; per-worker `continuationHistory[2][2]` keyed by `[inCheck][capture]` (4 tables). Stack linkage sets `ss->continuationHistory` per move; move loop references **6 plies** `contHist[0..5] = (ss-1)..(ss-6)`. Update weights `{{1,1133},{2,683},{3,312},{4,582},{5,149},{6,474}}` (offset, weight/1024); in-check nodes only update plies 1–2. Gravity formula `val + clamp(bonus,-D,D) - val*|bonus|/D`, `D=30000`.
  - **Ethereal (`thread.h:50`, `history.c:66-92`):** `NodeState.continuations` pointer to `[CONT_NB][PIECE][SQUARE]`; per quiet move sums `[0]` counter (ss-1), `[1]` follow-up (ss-2), `[2]` butterfly+threats. `NULL_HISTORY` sentinel for missing CM/FM. History divisor 16384.
  - **Weiss (`threads.h:80`, `history.h:32`):** `continuation[2][2]` (4 tables by `[inCheck][capture]`); move ordering uses offsets **1, 2, 4** (not 1,2); updates apply to 1, 2, 4. Bonus `min(2418, 251*depth-267)`, Malus `-min(693, 532*depth-163)`, divisor 16384.
  - **Leorik (`History.cs:99-106`):** `Continuation[ContDepth=4, 64, 14]` — tracks **4 prior plies**, deeper than the usual 2. Updated only on quiet cutoffs.
  - **Search-stack sizing implication:** Stockfish uses `Stack stack[MAX_PLY + 10]`, indexed `ss = stack + 7`, because continuation updates reach `ss-6`. Coco's stack must reserve enough negative headroom.

- **Capture History Sorting Heuristic:**
  - **Stockfish (`history.h:142`):** `CapturePieceToHistory = Stats<int16, 10692, PIECE_NB, SQUARE_NB, PIECE_TYPE_NB>` indexed `[movedPiece][to][capturedType]`. Initialized to **-689** (unknown captures slightly disfavored). Bonus `min(116*depth-81,1515) + 347*(bestMove==ttMove) + (ss-1)->statScore/32`, scaled `1395/1024`; malus `-min(848*depth-207,2446) + 17*moveCount`, scaled `1448/1024`. Feeds SEE margin `max(166*depth + captHist/29, 0)`.
  - **Ethereal (`history.c:51-64`):** `chistory[piece][threat_from][threat_to][to][captured]` — capture history is *threat-sliced* (4× the granularity). Queen promotions inflated by 64000; MVV-LVA augment `{0,2400,2400,4800,9600}` added for non-negative sort scores. Drives noisy LMR `R = 3 - hist/4952`.
  - **Weiss (`history.h:31`):** `captureHistory[PIECE_NB][64][TYPE_NB]`, divisor 14387.
  - **Leorik:** not implemented — captures ordered by MVV-LVA only. A gap that confirms Coco's separate capture-history table is the right direction.

- **Syzygy Endgame Tablebases:**
  - **Fathom Integration:** Fathom is batteries-included (possesses its own internal movegen/attacks, inline guards rejecting `castling≠0 || rule50≠0`, and computes `tbScore` + PVs + DTM expansion).
  - **Probe preconditions (critical):** in-search WDL probe only when `pieces ≤ TB_LARGEST` AND `rule50==0` AND `no castling rights`. Root DTZ probe may pass rule50 through. Fathom's inline wrapper enforces this.
  - **`dtz_to_wdl` threshold = 100** (`dtz>0 → dtz+cnt50<=100 ? WIN : CURSED_WIN`). `WdlToDtz={-1,-101,0,101,1}` for zeroing-move DTZ derivation.
  - **Root move ranking (Fathom `tbprobe.c:2387-2389`):** win → `dtz+cnt50<=99 && !hasRepeated ? 1000 : 1000-(dtz+cnt50)`; loss → `-dtz*2+cnt50<100 ? -1000 : -1000+(-dtz+cnt50)`; draw → 0. The `hasRepeated` flag downgrades guaranteed wins when repetition is possible.
  - **Stockfish integration (`search.cpp:802-854`, `tbprobe.cpp:1603-1676`):** WDL probed at non-root leaf nodes; maps to `±(VALUE_TB - ply)` with `drawScore = useRule50 ? 1 : 0`; writes TT and returns if exact or fails high/low. Root `rank_root_moves` disables in-search probing (`config.cardinality = 0`) if DTZ available or already winning.
  - **Ethereal/Weiss wrappers:** depth gate only blocks the *largest* TB below `TB_PROBE_DEPTH` so smaller TBs are always probed; PvNode clamps `syzygyMin/syzygyMax`; DTZ drives root move selection only when `!limitedByMoves && multiPV==1`.
  - **Interface-first principle (reaffirmed):** keep this as a thin adapter (WDL/DTZ probe + rule50 + root preselection), not a search rewrite.

### Tier 6

- **SEE Capture Pruning Gating:**
  - **Stockfish `see_ge` (`position.cpp:1295-1397`):** a boolean *threshold comparator* (not a value calc). Early exits: `PieceValue[captured] - threshold < 0 → false`; `PieceValue[attacker] - swap <= 0 → true`. Reveals X-ray attackers behind captured sliders; handles pinned `blockers_for_king`; castling/promo/ep assume SEE passes.
  - **Gates:** captures/checks margin `max(166*depth + captHist/29, 0)`; quiets `-25*lmrDepth*lmrDepth` (quadratic); qsearch futility `alpha - futilityBase`, general capture `-80`.
  - **Ethereal (`search.c:638-642`):** gate `!SEE(move, seeMargin[isQuiet] - hist/128)` with `seeMargin[quiet]=-64*depth`, `seeMargin[noisy]=-20*depth*depth`. History is *subtracted from the margin* → good-history moves get a laxer SEE bar. **Stage gate** (`> STAGE_GOOD_NOISY`) avoids re-running SEE on already-classified good captures.
  - **Weiss (`search.c:478-479`):** `lmrDepth<7 && !SEE(pos, move, -73*depth)`. QS gate threshold `1`. Stage-based break after `NOISY_GOOD`.
  - **Concept:** SEE is a cheap boolean gate used at multiple points with depth/history-scaled *negative* margins — the more negative, the deeper/later the move is allowed through. Gate SEE pruning late enough that TT/PV context can still protect tactical resources.

- **History-Based Alpha Pruning:**
  - **Stockfish (`search.cpp:1084-1090`):** `history = contHist[0] + contHist[1] + pawnHistory`; prune quiet if `history < -4083*depth`. Also `lmrDepth += history/3208` (good history deepens, bad further reduces) feeding futility/SEE gates.
  - **Weiss (`search.c:474-475`):** `quiet && lmrDepth<3 && histScore < -1024*depth → continue`. Plus a **`doPruning` time-gate** (`search.c:675-679`): pruning only engages after a time/depth threshold so early iterations search fully.
  - **Concept (confirmed):** tie history pruning to continuation/correction history (not a single raw number), use a depth-scaled negative threshold, and gate by time so early iterations aren't starved.

- **Multicut Search Evaluation:**
  - **Stockfish (`search.cpp:1154-1164`):** a *single-exclude-search* variant — reuses the singular verify search. If excluding the ttMove *still* fails high over `beta` (and `!is_decisive(value)`), return `value` as a softbound and penalize ttMove history `<< max(-400-100*depth, -4000)`. No separate multi-move search.
  - **Ethereal (`search.c:677-678`):** if verify fails high and `rBeta >= beta`, force picker to `STAGE_DONE` and return `max(ttValue - depth, -MATE)` — reuses the existing picker, no separate move list.
  - **Weiss (`search.c:509-510`):** direct `return singularBeta`.
  - **Concept:** multicut = the third branch of the singular decision (singular / multicut / negative-extension). Keep the trigger narrow, depth-limited, non-decisive, and reuse the verify search — never a separate broad multi-move probe.

- **Correction History Tables (CorrHist):**
  - **Stockfish (`history.h:160-211`):** split into `Pawn, Minor, NonPawn, PieceTo, Continuation`. Bundle of 4 *atomic* entries (`pawn, minor, nonPawnWhite, nonPawnBlack`) keyed by structure hashes (`pawn_key`, `minor_piece_key`, `non_pawn_key(c)`) masked to `CORRHIST_BASE_SIZE=65536`; `CORRECTION_HISTORY_LIMIT=1024`. Correction = weighted sum `10347*pawn + 8821*minor + 11665*(nonPawnW+nonPawnB) + 7841*cntcv`, applied as `clamp(rawEval + cv/131072, ...)`. Feeds singular margins, futility margin, LMR reduction (`r -= |cv|/30370`), aspiration. **Atomic + NUMA-shared** via `SharedHistories`.
  - **Weiss (`history.h:33-54, 124-167`):** 6 tables (`pawnCorr`, `minorCorr`, `majorCorr`, `nonPawnCorr[COLOR]`, `contCorr`), each 16384 entries keyed by separate Zobrist keys. `CorrectionBonus = clamp((score-eval)*depth/4, -172, 289)`; weighted-sum blend with hand-tuned coefficients `/131072`. **Update guard:** skip if inCheck, bestMove capturing, fail-high `<= staticEval`, or no bestMove `>= staticEval`. Also **rule50 scaling** of corrected eval (`rule50>7 → *= (256-rule50)/256`).
  - **Leorik (`History.cs:26-169`):** distinct statistical design — `CorrEntry{Numerator,Denominator}`, `Get = Numerator/(Denominator+100)`, `CORR_TABLE=19997` (prime), 8 buckets = 2 stm × 4 piece-groups (Pawns / Minors / Majors / Kings), each group's *bitboard* hashed mod-prime. Adjusted eval used everywhere in lieu of raw eval. Update guard suppresses positive adjustments on fail-lows and negative on fail-highs.
  - **Ethereal:** no CorrHist at all — a confirmed low-risk Elo gain opportunity for Coco.
  - **Concept (confirmed):** correct different eval contexts separately (pawn / minor / non-pawn / continuation), blend with tuned weights over a single `/131072` (or similar) scale, guard updates against reinforcing wrong directions, and consider NUMA-sharing if Coco goes multi-threaded.

- **Threat-Input Neural Network Architectures:**
  - **Stockfish `Full_Threats` (`nnue/features/full_threats.h/.cpp`, `position.cpp:1111-1213`):** `Dimensions=79856`, `MaxActiveDimensions=128`. Threats maintained *incrementally* as a `DirtyThreats` list (capacity 96) updated from `put_piece`/`remove_piece`/`move_piece`/`swap_piece` on every `do_move`. Outgoing threats = `attacks_bb(pc,s,occ) & occ`; incoming = sliders + knights/pawns/king attacking `s`; **discovered threats** via `RayPassBB`/`BetweenBB` when a slider moves. AVX512ICL fast path builds up to 16 dirty entries in one go.
  - **King bucketing for threats = only 2 buckets** (file-half, a-d vs e-h), via bit-2 of the king square — far coarser than PSQ's buckets. Refresh required only on king half-board crossing.
  - **Dual accumulators** (`nnue_accumulator.h:199-200`): both `psq_accumulators` and `threat_accumulators`. Threats are a *parallel input stream summed into the same L1 neurons pre-activation* with their own int8 weights + PSQT skip (halved: `(psqt + threatPsqt[p0] - threatPsqt[p1])/2`). Because threats are added *before* the squared activation, threat presence amplifies/suppresses PSQ activation non-linearly. **Threats have no accumulator cache** (always full/incremental recompute).
  - **Threats are big-net-only** (`UseThreats = (L1==1024)`); the small net (128) uses PSQ only.
  - **Feature index construction:** compile-time multi-level LUT; `map[attacker][attacked]` excludes nonsensical pairs (e.g. pawn→king) with `-1`.
  - **Bullet:** no threat inputs exist out of the box — if Coco wants threat-aware NNUE it must implement its own `SparseInputType`. This is a custom-training-pipeline task, not a config flip.

- **Multi-Network Architectures (Dual/Quad Networks):**
  - **Stockfish (`evaluate.cpp:43-90`):** two nets, `NetworkBig` (L1=1024, threats) + `NetworkSmall` (L1=128, PSQ-only), both NUMA-replicated. Routing: `use_smallnet = |simple_eval| > 962` where `simple_eval = PawnValue*(pawn diff) + non_pawn_material diff`. **Confidence-based fallback:** if small net returns `|nnue| < 277`, re-evaluate with the big net. Complexity blending: `nnueComplexity = |psqt - positional|`; `optimism += optimism*complexity/476`; `nnue -= nnue*complexity/18236`; material-scaled `(nnue*(77871+material) + optimism*(7191+material))/77871`; rule50 damping `v -= v*rule50/199`. Net naming `nn-[SHA256 12].nnue` is a hard contract with the test infra.
  - **Ethereal (`evaluate.c:449`):** NNUE unless `|ScoreEG(psqtmat)| > 2000` → classic HCE + separate **PKNet** (224×32×2) for pawn+king structure, cached in per-thread `PKTable`. Routing by material imbalance, not position class.
  - **Leorik:** single net, 5 input buckets + 8 output buckets within one file; ships a Layer1Size scaling study (1HL→768HL).
  - **Bullet routing primitives:** `MaterialCount<N>` output buckets = `(popcount(occ)-2)/div_ceil(32,N)`; `ChessBucketsMirrored` input buckets with file pattern `[0,1,2,3,3,2,1,0]` halving king-bucket params; **factoriser** trick — a small shared 768-wide matrix added to every bucket's weights to improve generalization, folded into bucketed weights at save time (`merge_factoriser`). Dual-perspective via `dual_perspective()` (STM + NTM with `sq^56` flip, shared FT).
  - **Concept (confirmed):** treat dual-net as a *routing problem first* — small fast net for obviously-decided positions (large material/score imbalance), big net for complex/near-zero positions, with a confidence-based fallback. Use piece-count output buckets and king-square input buckets; consider a factoriser to cut parameters.

- **Distributed Testing Framework (OpenBench Server Setup):**
  - **Stockfish uses Fishtest (not OpenBench), but the engine-side hooks transfer:** deterministic `bench` command on a fixed position suite for regression/cheat detection; `Skill Level`/`UCI_LimitStrength`/`UCI_Elo` (1320–3190, anchored to Stash via a cubic fit) for handicap tests; `nodestime` for node-budgeted testing; a `tune.cpp` import path for fishtest SPRT constants; `TT::new_search()` generation aging + a tiny nodes-dependent `value_draw` perturbation to avoid 3-fold blindness; `NNUE_EMBEDDING_OFF` escape for MSVC profile builds.
  - **Weiss:** `bench` subcommand, `tuner.c` under `TUNE` macro, `DEV` mode (`eval`/`print`/`perft`), `.github/workflows/make.yml` CI, `OnlineSyzygy` + `noobprobe` cloud books, `HasCycle` via cuckoo hashing detecting *upcoming* repetitions (anti-repetition-blindness), `DrawScore` with low-bit variance.
  - **Ethereal:** `bench.csv`, perft EPDs, `tuner.c/h` + `Tuning.pdf`, `Normalize` UCI option scaling cp so +1.00 ≈ 50% (`100*bounded/186`), inline per-step Elo estimates annotated in `search.c`.
  - **Concept:** preserve Coco's disciplined testing funnel; expose the hooks distributed workers need (deterministic `bench`, strength-scaling, `nodestime`, generation aging, reproducible draw handling, net-naming convention, tuning import path).

---

## 📝 Key Deferred Decisions & Future Design Heuristics

### 1. Movegen Optimization: Caching, Precomputing, and "No SIMD"
* **"Remove unnecessary SIMD":** Keep move generation strictly bitboard-based. A chessboard fits perfectly within single 64-bit CPU registers (`uint64_t`). Using SIMD for movegen introduces vector startup latency and register shuffling overhead, which hurts rather than helps NPS. Keep SIMD (AVX2/Neon) dedicated to NNUE evaluation only.
* **Modern Bit Manipulation Builtins:** Instead of platform-dependent inline assembly, use C++20/C++23 standard library bit manipulation features (`<bit>`) or compiler intrinsics (like `__builtin_ctzll` / `popcount`). Compilers natively transform these into the corresponding hardware-accelerated instructions (`tzcnt`, `popcnt`).
* **Expand Precomputations:** Precompute helper ray tables at startup:
  * **Line Bitboards:** `line_bb[sq1][sq2]` storing the full line passing through two squares.
  * **Between Bitboards:** `between_bb[sq1][sq2]` storing only the squares strictly between two squares (ideal for blocker checks).
  * **King Safety Rings:** Mask arrays representing ring boundaries surrounding the king.

### 2. Search Heuristics & SPSA Tuning
* **Aggressive Pruning:** Search depth improvements are driven by pruning unpromising branches early (discarding ~95% of moves at low depths) rather than brute-forcing full calculations.
* **Late Move Pruning (LMP):** Once a search node exceeds a threshold of searched quiet moves (e.g., `move_count > 3 + depth * depth`), prune all remaining quiet moves at low depths.
* **History-Based Pruning:** Prune quiet moves presenting very low history scores.
* **SEE Capture Pruning in PVS:** Extend Static Exchange Evaluation (SEE) pruning from Quiescence Search to the main PVS search loop to reject captures that lose material.
* **SPSA Parameter Tuning:** Parameterize all new pruning thresholds and margins (e.g., `margin = A * depth + B`) and run SPSA tuning cycles (via `tuning/tune_30k.py`) to optimize values mathematically.

### 3. History-Based Alpha Pruning & Neural Network (NNUE) Interdependencies
* **History-Based Pruning Pre-requisite:** Raw history-based pruning is too crude on its own and leads to tactical blindness. A robust history pruning system requires active **Correction History (CorrHist)** and **Continuation History (CMH/FMH)** to scale negative thresholds dynamically based on pawn/piece contexts.
* **NNUE Brain Expansion (L1 = 512 / 1024):** Upgrading to a larger network increases evaluation sensitivity. Without Correction History to smooth out evaluation variance and stabilize search fluctuations, a larger network can cause search regressions. Active CorrHist is a prerequisite to successfully utilizing a larger brain.
* **Singular Extension Margins:** Singular extension margins (currently `depth * (is_pv ? 1 : 2)`) should eventually scale dynamically based on Correction History and Continuation History feedback to reach modern top-tier standards.

### 4. Deferred Search & History Architecture Choices
* **LMR Adjustment via Continuation History:** Deferred to avoid search instability. Move ordering improvements are prioritized first to establish a clean, verified baseline before modifying late move reduction values.
* **[inCheck][capture] 4-Table Split:** Deferred due to high memory footprint. Splitting continuation history into 4 tables (`[2][2]`) increases the memory cache footprint (up to ~3.1 MB per thread), adding significant cache overhead for negligible Elo gains compared to a single-sliced `[2][7][64][7][64]` table (~784 KB per thread).

### 5. Established NNUE Compatibility & Local Testing Option
* **Compatibility Reference:** Coco's neural network utilizes a standard **`768 -> 256x2 -> 1`** architecture with **SCReLU** activation (`(clamped_x * clamped_x) / 255`). This format is mathematically identical to the networks of **Leorik 2.x** and **Weiss v1.x / v2.x**.
* **Local Strength Testing:** To evaluate Coco's search strength independent of our custom network training stage, we can locally load a trained network from **Leorik 2.1** or **Weiss** by renaming the weights file to `coco.nnue`. 
* **Quantization Alignment:** If we load an external net and observe evaluation anomalies, we may need to write a simple conversion/rescaling script (e.g., to scale layer 1/2 weights and biases) to align their quantization constants with Coco's internal scaling factors.

