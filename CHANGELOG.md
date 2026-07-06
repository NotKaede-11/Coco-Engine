# Changelog

All notable changes to the Coco Chess Engine will be documented in this file.

## [1.0.1] - 2026-07-06

### Added
- **Dynamic UCI Option (`Move Overhead`):** Added option to customize the clock latency buffer dynamically (default: `30` ms).
- **Dynamic UCI Option (`EvalFile`):** Added option to swap and load new neural network weight files (`.nnue`) at runtime without recompilation.
- **NNUE Directory Fallback Loader:** Added automatic fallback path search: if `coco.nnue` is not found in the active working directory, the engine will look next to the running executable.
- **Fail-Fast Engine Startup:** The engine will now exit immediately with code `1` and print an error if `coco.nnue` fails to load, preventing silent "zero-eval" play.
- **Static Compilation build setup:** Embeds all C++ standard runtime dependencies (`libgcc`, `libstdc++`, `winpthreads`) directly inside the executable, removing external DLL requirements.

### Fixed
- **Quantized L2 Weights Swapping:** Fixed the layer 2 perspective weights swapping bug in the Bullet checkpoint conversion script (`scripts/export_bullet_to_nnue.py`), aligning bullet-quantized outputs with the engine's active-passive accumulator indexing.
- **Fullmove Clock Drift:** Fixed a minor state update tracking bug where the fullmove counter drifted on specific move rollbacks.

### Changed
- **Repository Reorganization:** Streamlined the root directory. Restructured test binaries and fastchess into `testing/`, and SPSA scripts and configurations into `tuning/`.
