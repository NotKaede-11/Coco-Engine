# Coco Chess Engine

Coco is a high-performance, neural-network-evaluated (NNUE) chess engine written in C++23. It is designed to combine a fast, search core with a deep, position-aware neural network that evaluates the board in constant time.

## A Note on Development

This engine is a project I developed through close with AI. I treat Coco as an evolving piece of software; it is actively being trained, tested, and improved. Therefore, I do not consider it as "work" per se, but rather as a personal project that I am continuously trying to improve. I welcome feedback, suggestions, and criticims from the community as I am aware that most people do not view the who "AI coded" thing positively. Still, it is something I prompted out of the blue because I got bored and wanted to see how far can a hands free engine development would go in terms of its strenth.

## Why the name?

The name Coco comes from the manga *Witch Hat Atelier*. I love the MC of the show and I just wanted to use her name for this project. 

## Core Architecture

Coco avoids the limitations of classical engines that rely on manual, rule-of-thumb heuristics. Instead, it uses a quantized neural network to understand board states.

*   **Board Representation**: The engine uses bitboards (64-bit integers) for rapid board analysis.
*   **Move Generation**: It utilizes Magic Bitboards for sliding pieces, allowing for nearly instant retrieval of attack vectors.
*   **Asynchronous Core**: The search core is built on Principal Variation Search (PVS) and utilizes Zobrist hashing in a flat, contiguous Transposition Table to avoid redundant work.

## Search & Pruning Heuristics

To evaluate millions of positions per second, Coco uses an aggressive suite of pruning and reduction techniques:

*   **Pruning Suite**: I’ve implemented Null Move Pruning (NMP), Reverse Futility Pruning (RFP), and Razoring to aggressively drop branches that don't look promising.
*   **Reductions & Ordering**: The engine uses Late Move Reductions (LMR) for quiet moves, while prioritizing "Killer", "History", "Contextual Continuation History (CMH + FMH)", and "Capture History" moves.
*   **On-Demand Threats**: Dynamic enemy attack maps penalize quiet moves stepping into heavily defended lines.
*   **Lazy SMP Multithreading**: Spawns multiple worker threads sharing a lockless Transposition Table (TT) for scale.
*   **Iterative Deepening**: Coco searches incrementally, ensuring the Transposition Table is primed with the best moves before deeper, more exhaustive searches begin.
*   **Aspiration Windows**: The engine uses a narrow scoring window to focus the search. If a score fails to fit within this window, the engine dynamically widens it and re-searches.

## The NNUE Brain

Coco’s "brain" is a HalfKP neural network.

*   **The Setup**: It tracks 768 input features (piece positions relative to the friendly king) and processes them through an incremental accumulator. This means evaluation happens in constant $O(1)$ time because I update only the changes made by the move rather than re-evaluating the whole board. Supports larger L1 architecture sizes (512/1024).
*   **Training**: The network was trained using PyTorch on 1.5 million quiet positions.
*   **Quantization**: To keep things lightning-fast on your CPU, I quantized the weights into integers. This avoids floating-point overhead and keeps the math strictly in the realm of fast integer operations. Optimized using vectorized AVX2 intrinsics.

## Time Management

I developed an Elastic Clock for Coco to handle time pressure smartly:

*   **Soft Limit**: A balanced budget per move.
*   **Hard Limit**: A strict safety threshold that interrupts the search if we are running out of time, ensuring the engine never flags in a winning or drawn position.

## UCI Support

Coco communicates via the standard UCI (Universal Chess Interface) protocol. You can adjust the Hash size, control Threads, swap evaluating weights file (`EvalFile`), or tweak the search parameters directly through your GUI's settings.

## How to Compile

To compile and run Coco from source:

### Windows (GCC / MinGW)
The recommended way to compile on Windows is to run the provided batch file in the project directory:
```cmd
build.bat
```
This automatically generates the embedded network header and compiles the engine. Alternatively, you can compile manually by running:
```bash
python scripts/make_nnue_header.py
g++ -O3 -march=native -pthread -static -std=c++23 src/*.cpp Fathom/src/tbprobe.c -IFathom/src -o coco-chess.exe
```

### Linux
Open your terminal in the project directory and run:
```bash
python scripts/make_nnue_header.py
g++ -O3 -pthread -std=c++23 src/*.cpp Fathom/src/tbprobe.c -IFathom/src -o coco-chess
```

### macOS (Intel)
Open your terminal in the project directory and run:
```bash
python scripts/make_nnue_header.py
g++ -O3 -pthread -std=c++23 src/*.cpp Fathom/src/tbprobe.c -IFathom/src -o coco-chess
```

### macOS (Apple Silicon M1/M2/M3)
Open your terminal in the project directory and run:
```bash
python scripts/make_nnue_header.py
g++ -O3 -pthread -std=c++23 -D__ARM_NEON src/*.cpp Fathom/src/tbprobe.c -IFathom/src -o coco-chess
```

Once compiled, you can run the engine executable and interact with it using standard UCI commands, or load it into any chess GUI (such as Cutechess, Arena, or Lichess-bot).
