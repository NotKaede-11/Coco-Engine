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

## Coco 1.5.1

**Coco 1.5.1** is a compatibility hotfix based on official v1.5.0. It starts
with its embedded network even when older Coco versions share the same folder.
`EvalFile` defaults to `<embedded>`; external networks require an explicit path.
An invalid external-file request warns and retains the active weights.

Search and the 512-unit network are unchanged from v1.5.0. Experimental 1.6
changes are not included. The following strength evidence belongs to v1.5.0,
not a new hotfix strength test.

**Official CCRL 40/15 Rating: 2994 Elo** (+99/−99, 61.5% score over 26 games on the [CCRL 40/15 rating list](https://computerchess.org.uk/4040/cgi/engine_details.cgi?print=Details&each_game=0&eng=Coco%201.5.0%2064-bit#Coco_1_5_0_64-bit)). In local filtered Ordo calibration at `10+0.1`, Coco scored **52.75% over 4,442 retained games** (~2,977 Elo). The [gauntlet report](docs/evidence/v1.5.0-gauntlet.md) retains all 5,000 original results, the filtered rating table, uncertainty and limitations.

See the [hotfix notes](docs/releases/v1.5.1.md) and [release checklist](docs/RELEASING.md).

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

## Inside Coco

<table>
  <tr>
    <td width="50%" valign="top">
      <strong>Board and move generation</strong><br><br>
      64-bit bitboards, Zobrist hashing, checked make/unmake state, compact incremental position fingerprints, magic sliding attacks, and an optional BMI2/PEXT backend. Dedicated capture, quiet, and evasion generation feeds a staged MovePicker.
    </td>
    <td width="50%" valign="top">
      <strong>Search</strong><br><br>
      Iterative deepening, aspiration windows, PVS, quiescence search, transposition-table cutoffs, null-move pruning, reverse futility pruning, razoring, late-move reductions, histories, guarded capture ProbCut, explicit principal variations, root-aware time allocation, and guarded extensions.
    </td>
  </tr>
  <tr>
    <td width="50%" valign="top">
      <strong>Neural evaluation</strong><br><br>
      A quantized Chess768 NNUE with 512 hidden units per perspective is embedded into release binaries. Both perspectives feed the output layer. Incremental accumulators update only the features changed by each move.
    </td>
    <td width="50%" valign="top">
      <strong>Parallel search</strong><br><br>
      Lazy SMP workers share a lockless transposition table while retaining independent search state and per-thread node accounting and best-move voting when multiple workers are active.
    </td>
  </tr>
  <tr>
    <td width="50%" valign="top">
      <strong>Analysis and endgames</strong><br><br>
      MultiPV, ponder support, external evaluation files, and optional Fathom/Syzygy probing for tablebase positions.
    </td>
    <td width="50%" valign="top">
      <strong>Testing discipline</strong><br><br>
      Perft, state restoration, deterministic fixed-depth tests, timed A/B comparisons, noisy-position regression suites, sanitizers, and paired SPRT are used before behavioral changes are accepted.
    </td>
  </tr>
</table>

### Pure calculation

Coco contains no built-in opening book and does not consult one during normal UCI play. Its moves come from search.

Development matches may give both engines the same external opening suite. This creates varied, reproducible starting positions without adding book knowledge to the released engine.

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

Eight internal search options remain accepted through `setoption`, but are hidden from GUI discovery: `RFP_Margin`, `LMR_Constant_Scaled`, `NMP_Base`, `NMP_Divisor`, `Aspiration_Delta`, `History_Threshold`, `LMR_History_Divisor`, and `SEE_Pruning_Depth`.

The network is 789,508 bytes with SHA-256 `392BE46C8E06C6D0CB6BEDF00D8E3D08950D11DAA883362DE98F0C1DEEE68055`. Files beside the executable do not override the embedded net automatically. Select an external network explicitly using `EvalFile`; use a unique filename such as `coco-512x2-392be46c8e06.nnue` to avoid collisions. Set `<embedded>` to restore the built-in weights. Failed explicit loads preserve the current network. Use the `info string build` diagnostic to verify the active network when comparing results.

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

## Testing and development

Coco treats reproducible evidence as part of implementation. A behavioral candidate must preserve correctness before its speed or playing strength is considered.

```bash
python testing/validate_options.py ./coco-chess
python testing/verify_uci_limits.py ./coco-chess
python testing/run_perft_suite.py 3
python testing/run_cpp_tests.py
python testing/noisy_opening_regression.py --engine ./coco-chess
```

<details>
<summary><strong>Self-play data generation</strong></summary>

```bash
coco-chess --datagen 1000000 8 training.bin --seed 20260808
```

The arguments are target positions, worker threads, and output path. Additional controls include `--buffer`, `--datagen-tt`, `--manifest`, and an explicitly opt-in external `--book` input. Each dataset records its engine, network, seed, schema, adjudication settings, and exact record count; incompatible resumes are refused.

</details>

Search tuning requires a runner configured for the hidden internal parameters and a frozen engine/network baseline. Keep tuning probes separate from an independent candidate-versus-baseline test.


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
