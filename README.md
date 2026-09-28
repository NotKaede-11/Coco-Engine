<div align="center">

  <h1>Coco Chess Engine</h1>

  <img src="assets/logo.png" alt="Coco Chess Engine" width="900">

  <br>
  <br>

  **A modern, free UCI chess engine built around verified search and efficient NNUE evaluation.**

  [![Release: v1.5.1][release-badge]][release-link]
  [![License: GPL v3][license-badge]][license-link]
  [![C++20][cpp-badge]][source-link]
  [![CCRL 40/15: 2994](https://img.shields.io/badge/CCRL%2040%2F15-2994-blue?style=flat-square)](https://computerchess.org.uk/4040/cgi/engine_details.cgi?print=Details&each_game=0&eng=Coco%201.5.0%2064-bit#Coco_1_5_0_64-bit)

  [Official website](https://notkaede-11.github.io/coco-engine-website/) · [Download][release-link] · [Quick start](#quick-start) · [Build](#build-from-source) · [Changelog](CHANGELOG.md)

</div>

---

**Coco Chess Engine** is a free, open-source, cross-platform UCI chess engine created and maintained by [NotKaede-11](https://github.com/NotKaede-11). Its search combines bitboards, staged move ordering, parallel alpha-beta techniques, an incrementally updated neural evaluator, and optional Syzygy tablebase probing.

> [!IMPORTANT]
> Coco is an engine, not a graphical chess application. Use it through a UCI-compatible interface such as Arena, BanksiaGUI, Cute Chess, or another chess GUI.

## Strength

| Version | My Estimate | CCRL 40/15 | CCRL Blitz |
| :---: | :---: | :---: | :---: |
| 1.5.1 | 2977 | - | - |
| 1.5.0 | 2977 | [2994](https://computerchess.org.uk/4040/cgi/engine_details.cgi?print=Details&each_game=0&eng=Coco%201.5.0%2064-bit#Coco_1_5_0_64-bit) | - |
| 1.4.0 | - | - | - |
| 1.3.0 | - | - | - |
| 1.1.1 | - | - | [2595](https://computerchess.org.uk/404/cgi/engine_details.cgi?print=Details&each_game=1&eng=Coco%201.1.1%2064-bit#Coco_1_1_1_64-bit) |
| 1.1.0 | - | - | - |
| 1.0.1 | - | - | - |
| 1.0.0 | - | - | - |

## Quick start

1. Download a published binary for your platform from the [releases page][release-link].
2. Choose the binary that best matches your CPU using the table below.
3. Add the executable as a UCI engine in your chess GUI.

To check the engine directly from a terminal, start it and enter:

```text
uci
isready
position startpos
go movetime 1000
```

Wait for `bestmove`, then enter `quit`.

## Choose the right binary

| Platform | Build | Best fit |
|:--|:--|:--|
| Windows / Linux | `x86-64-bmi2` | Modern Intel and AMD Zen 3 or newer; recommended for most recent x86 systems |
| Windows / Linux | `x86-64-avx2` | AMD Zen 1/2 or systems where BMI2 `pext` is relatively slow |
| Windows / Linux | `x86-64-avx512` | CPUs with AVX-512F, BW, DQ, and VL support |
| Windows / Linux | `x86-64-popcnt` | Older 64-bit x86 CPUs with SSE4.1 and POPCNT |
| macOS | `apple-silicon` | Apple M-series processors |
| macOS | `x86-64-avx2` / `x86-64-popcnt` | Intel-based Macs |
| Linux ARM | `arm64` / `arm64-dotprod` | ARMv8 systems, with dot-product build for supported ARMv8.2+ CPUs |

Using instructions unsupported by your CPU will prevent the engine from starting. When uncertain, choose the POPCNT or baseline ARM64 build.

## Features

| Component | Architecture & Highlights |
|:--|:--|
| **Board & Movegen** | 64-bit bitboards, magic bitboards with optional BMI2/PEXT, staged move ordering |
| **Search** | Alpha-beta PVS, iterative deepening, aspiration windows, RFP, NMP, LMR, and singular extensions |
| **Neural Evaluation** | Embedded 768→512×2 NNUE with SIMD-vectorized incremental accumulator updates |
| **Concurrency** | Lazy SMP multithreading across CPU cores sharing a lockless transposition table |
| **Endgame & Tools** | MultiPV, ponder support, and integrated Syzygy tablebase probing (up to 6-piece) |

## UCI configuration

The release build identifies as `Coco v1.5.1` and exposes these fifteen options to chess GUIs:

| Option | Default | Purpose |
|:--|--:|:--|
| `Hash` | 16 MiB | Transposition-table memory |
| `Clear Hash` | button | Clear the transposition table |
| `Threads` | 1 | Parallel search workers |
| `Ponder` | false | Think during the opponent's turn |
| `MultiPV` | 1 | Number of principal variations to report |
| `Move Overhead` | 30 ms | Safety allowance for GUI and operating-system latency |
| `Use PEXT` | hardware dependent | Use the BMI2 sliding-attack backend when supported |
| `EvalFile` | `<embedded>` | Use built-in weights; set an explicit file path to load a compatible external network |
| `SyzygyPath` | empty | Path to Syzygy tablebase files |
| `SyzygyProbeDepth` | 1 | Depth threshold for probing positions at the largest loaded tablebase size |
| `SyzygyProbeLimit` | true | Apply that depth threshold at the largest loaded tablebase size |
| `Syzygy50MoveRule` | true | Respect the 50-move rule in root tablebase decisions |
| `UCI_ShowWDL` | false | Include win/draw/loss permille estimates in analysis output |
| `UCI_AnalyseMode` | false | Accept the standard GUI analysis-mode signal |
| `Contempt` | 0 | Optional draw-preference setting; neutral by default |

## Build from source

The build requires a C++20-capable compiler and Python for generating the embedded network header. Release builds embed `coco.nnue` into the executable.

### Windows

With GCC/MinGW available on `PATH`:

```cmd
build.bat
```

### Makefile builds

```bash
make build ARCH=x86-64-popcnt COMP=gcc
make build ARCH=x86-64-avx2   COMP=gcc
make build ARCH=x86-64-bmi2   COMP=gcc
make build ARCH=x86-64-avx512 COMP=gcc
make build ARCH=armv8         COMP=gcc
make build ARCH=armv8-dotprod COMP=gcc
make build ARCH=apple-silicon COMP=clang
```

Use `COMP=mingw` to cross-compile Windows binaries from Linux. Run `make help` for all supported targets and options.

## Project

Coco is a personal chess engine created, architected, and maintained by [NotKaede-11](https://github.com/NotKaede-11). Ideas are treated as hypotheses: changes are retained through correctness checks, deterministic comparisons, and self-play evidence rather than because another engine uses a similar technique.

The name comes from Coco, the protagonist of *Witch Hat Atelier*.

Bug reports, test games, code review, and constructive feedback are welcome through [GitHub Issues][issues-link].

## License

Coco is free software distributed under the [GNU General Public License v3][license-link]. If you distribute a modified binary, you must also make the corresponding source available under the GPL.

[release-badge]: https://img.shields.io/badge/release-v1.5.1-blue?style=flat-square
[release-link]: https://github.com/NotKaede-11/Coco-Engine/releases/tag/v1.5.1
[license-badge]: https://img.shields.io/github/license/NotKaede-11/Coco-Engine?style=flat-square&label=license
[license-link]: LICENSE
[cpp-badge]: https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus
[source-link]: src
[issues-link]: https://github.com/NotKaede-11/Coco-Engine/issues
