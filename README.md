<div align="center">

  <img src="assets/logo.png" alt="Coco Chess Engine" width="900">

  <br>
  <br>

  **A modern, free UCI chess engine built around verified search and efficient NNUE evaluation.**

  [![Pre-release][release-badge]][release-link]
  [![License: GPL v3][license-badge]][license-link]
  [![C++23][cpp-badge]][source-link]

  [Download][release-link] · [Quick start](#quick-start) · [Build](#build-from-source) · [Changelog](CHANGELOG.md)

</div>

---

Coco is a cross-platform chess engine written in C++23. Its search combines bitboards, staged move ordering, parallel alpha-beta techniques, an incrementally updated neural evaluator, and optional Syzygy tablebase probing.

> [!IMPORTANT]
> Coco is an engine, not a graphical chess application. Use it through a UCI-compatible interface such as Arena, BanksiaGUI, Cute Chess, or another chess GUI.

## Quick start

1. Download the pre-release binary for your platform from the [releases page][release-link].
2. Choose the binary that best matches your CPU using the table below.
3. Add the executable as a UCI engine in your chess GUI.

To check the engine directly from a terminal, start it and enter:

```text
uci
isready
position startpos
go movetime 1000
quit
```

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
      64-bit bitboards, Zobrist hashing, checked make/unmake state, magic sliding attacks, and an optional BMI2/PEXT backend. Dedicated capture, quiet, and evasion generation feeds a staged MovePicker.
    </td>
    <td width="50%" valign="top">
      <strong>Search</strong><br><br>
      Iterative deepening, aspiration windows, PVS, quiescence search, transposition-table cutoffs, null-move pruning, reverse futility pruning, razoring, late-move reductions, histories, and guarded extensions.
    </td>
  </tr>
  <tr>
    <td width="50%" valign="top">
      <strong>Neural evaluation</strong><br><br>
      A quantized 768-input, 512-hidden-unit NNUE is embedded into release binaries. Incremental accumulators update only the features changed by each move.
    </td>
    <td width="50%" valign="top">
      <strong>Parallel search</strong><br><br>
      Lazy SMP workers share a lockless transposition table while retaining independent search state and truthful per-thread node accounting.
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

The most useful options exposed to chess GUIs are:

| Option | Default | Purpose |
|:--|--:|:--|
| `Hash` | 16 MiB | Transposition-table memory |
| `Threads` | 1 | Parallel search workers |
| `Ponder` | false | Think during the opponent's turn |
| `MultiPV` | 1 | Number of principal variations to report |
| `Move Overhead` | 30 ms | Safety allowance for GUI and operating-system latency |
| `Use PEXT` | true | Use the BMI2 sliding-attack backend when supported |
| `EvalFile` | `coco.nnue` | Load a compatible external network |
| `SyzygyPath` | empty | Path to Syzygy tablebase files |
| `Syzygy50MoveRule` | true | Respect the 50-move rule in root tablebase decisions |
| `UCI_ShowWDL` | false | Include win/draw/loss permille estimates in analysis output |
| `UCI_AnalyseMode` | false | Accept the standard GUI analysis-mode signal |

`Clear Hash`, `SyzygyProbeDepth`, `SyzygyProbeLimit`, `Contempt`, and advanced search-tuning options are also available. The shipped defaults are the tested playing configuration.

## Build from source

The build requires a C++23-capable compiler and Python for generating the embedded network header. Release builds embed `coco.nnue` into the executable.

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

<details>
<summary><strong>Search-parameter tuning</strong></summary>

```bash
python scripts/spsa_tune_30k.py --dry-run
python scripts/spsa_tune_30k.py --concurrency 8
python scripts/spsa_tune_100k.py --concurrency 8 --tc 20+0.2
python scripts/spsa_tune_100k.py --resume
```

The tuning tools verify the engine's advertised UCI parameters before starting. HCE weights use the dedicated production-feature tuner; SPSA is reserved for numerical search parameters.

</details>

## Project

Coco is a personal, AI-assisted engine-development project. Ideas are treated as hypotheses: changes are retained through correctness checks, deterministic comparisons, and self-play evidence rather than because another engine uses a similar technique.

The name comes from Coco, the protagonist of *Witch Hat Atelier*.

Bug reports, test games, code review, and constructive feedback are welcome through [GitHub Issues][issues-link].

## License

Coco is free software distributed under the [GNU General Public License v3][license-link]. If you distribute a modified binary, you must also make the corresponding source available under the GPL.

[release-badge]: https://img.shields.io/github/v/release/NotKaede-11/Coco-Engine?display_name=tag&include_prereleases&sort=semver&style=flat-square&label=pre-release
[release-link]: https://github.com/NotKaede-11/Coco-Engine/releases
[license-badge]: https://img.shields.io/github/license/NotKaede-11/Coco-Engine?style=flat-square&label=license
[license-link]: LICENSE
[cpp-badge]: https://img.shields.io/badge/C%2B%2B-23-00599C?style=flat-square&logo=cplusplus
[source-link]: src
[issues-link]: https://github.com/NotKaede-11/Coco-Engine/issues
