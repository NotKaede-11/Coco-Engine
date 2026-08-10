# Coco Chess Engine — Makefile
# ===========================================================================
# Usage:
#   make build ARCH=x86-64-avx2          # Standard modern CPUs
#   make build ARCH=x86-64-bmi2          # Intel Haswell+ / AMD Zen 3+
#   make build ARCH=x86-64-avx512        # Intel Ice Lake+ / AMD Zen 4+
#   make build ARCH=apple-silicon        # Apple M1/M2/M3/M4
#   make build ARCH=armv8                # Linux ARM64 (Raspberry Pi 5, servers)
#   make build ARCH=armv8-dotprod        # Linux ARM64 with dot-product support
#   make build ARCH=x86-64-popcnt        # Legacy 64-bit x86 (broad compat.)
#   make build ARCH=native               # Auto-detect host CPU (local dev)
#
#   Cross-compile Windows .exe from Linux:
#   make build ARCH=x86-64-avx2 COMP=mingw
#   make build ARCH=x86-64-bmi2 COMP=mingw
#
#   Profile-Guided Optimization (optional, ~5-10% extra NPS):
#   make pgo ARCH=x86-64-avx2 COMP=gcc
#
#   Clean:
#   make clean
# ===========================================================================

# --- Engine name and source files ---
EXE       = coco-chess

SRCS      = src/main.cpp       \
            src/board.cpp      \
            src/movegen.cpp    \
            src/tt.cpp         \
            src/search.cpp     \
            src/evaluate.cpp   \
            src/uci.cpp        \
            src/nnue.cpp       \
            src/datagen.cpp

FATHOM    = Fathom/src/tbprobe.c
INCLUDES  = -IFathom/src

# --- Base flags (always applied) ---
CXXFLAGS  = -O3 -pthread -std=c++20 -DL1_SIZE=512 -DNDEBUG
LDFLAGS   = -pthread
PYTHON   ?= python3

# ===========================================================================
# Section 1: Architecture SIMD flags
# ===========================================================================

ifeq ($(ARCH),)
    ARCH = native
endif

ifeq ($(ARCH),x86-64-avx2)
    CXXFLAGS += -mpopcnt -msse4.1 -mavx2
else ifeq ($(ARCH),x86-64-bmi2)
    CXXFLAGS += -mpopcnt -msse4.1 -mavx2 -mbmi2
else ifeq ($(ARCH),x86-64-avx512)
    CXXFLAGS += -mpopcnt -msse4.1 -mavx2 -mbmi2 -mavx512f -mavx512bw -mavx512dq -mavx512vl
else ifeq ($(ARCH),x86-64-popcnt)
    CXXFLAGS += -mpopcnt -msse4.1
else ifeq ($(ARCH),apple-silicon)
    CXXFLAGS += -march=armv8.2-a+crypto+dotprod
else ifeq ($(ARCH),armv8)
    CXXFLAGS += -march=armv8-a
else ifeq ($(ARCH),armv8-dotprod)
    CXXFLAGS += -march=armv8.2-a+dotprod
else ifeq ($(ARCH),native)
    CXXFLAGS += -march=native
else
    $(error Unknown ARCH=$(ARCH). Valid: x86-64-avx2 x86-64-bmi2 x86-64-avx512 x86-64-popcnt apple-silicon armv8 armv8-dotprod native)
endif

# ===========================================================================
# Section 2: Compiler / platform selection
# ===========================================================================

ifeq ($(COMP),mingw)
    # Cross-compile Windows .exe from Linux using MinGW
    CXX      = x86_64-w64-mingw32-g++
    EXE     := $(EXE).exe
    LDFLAGS += -static -static-libgcc -static-libstdc++
    CXXFLAGS += -DWIN32
else ifeq ($(COMP),clang)
	CXX = clang++
else
    # Default: native GCC
    CXX = g++
    ifeq ($(OS),Windows_NT)
        # Native MinGW builds must carry their runtime dependencies.
        LDFLAGS += -static -static-libgcc -static-libstdc++
    else
        LDFLAGS += -static-libstdc++
    endif
endif

# ===========================================================================
# Section 3: Build rules
# ===========================================================================

.PHONY: build tuner pgo clean help

build:
	@echo "=== Building Coco (ARCH=$(ARCH), COMP=$(COMP)) ==="
	$(PYTHON) scripts/make_nnue_header.py
	$(PYTHON) scripts/make_build_info.py --arch "$(ARCH)" --compiler "$(if $(COMP),$(COMP),gcc)"
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRCS) $(FATHOM) -o $(EXE) $(LDFLAGS)
	@echo "=== Done: $(EXE) ==="

tuner:
	@echo "=== Building Coco production-trace HCE tuner ==="
	$(PYTHON) scripts/make_nnue_header.py
	$(PYTHON) scripts/make_build_info.py --arch "$(ARCH)" --compiler "$(if $(COMP),$(COMP),gcc)"
	$(CXX) $(CXXFLAGS) $(INCLUDES) src/tuner.cpp src/board.cpp src/movegen.cpp src/evaluate.cpp src/nnue.cpp -o coco-tuner $(LDFLAGS)
	@echo "=== Done: coco-tuner ==="

pgo:
	@echo "=== PGO Build: Stage 1 — Instrumented binary ==="
	$(PYTHON) scripts/make_nnue_header.py
	$(PYTHON) scripts/make_build_info.py --arch "$(ARCH)" --compiler "$(if $(COMP),$(COMP),gcc)"
	$(CXX) $(CXXFLAGS) -fprofile-generate $(INCLUDES) $(SRCS) $(FATHOM) -o $(EXE)-pgo $(LDFLAGS)
	@echo "=== PGO Build: Stage 2 — Collecting profile data via bench ==="
	printf "bench\nquit\n" | ./$(EXE)-pgo
	@echo "=== PGO Build: Stage 3 — Optimized binary using profile data ==="
	$(CXX) $(CXXFLAGS) -fprofile-use -fprofile-correction $(INCLUDES) $(SRCS) $(FATHOM) -o $(EXE) $(LDFLAGS)
	@rm -f $(EXE)-pgo *.gcda *.gcno
	@echo "=== Done: $(EXE) (PGO optimized) ==="

clean:
	@echo "=== Cleaning build artifacts ==="
	rm -f $(EXE) $(EXE).exe $(EXE)-pgo *.gcda *.gcno
	@echo "=== Clean done ==="

help:
	@echo ""
	@echo "Coco Chess Engine — Build System"
	@echo ""
	@echo "Targets:"
	@echo "  build   Build the engine (default target)"
	@echo "  tuner   Build the production-trace HCE AdaGrad tuner"
	@echo "  pgo     Build with Profile-Guided Optimization (~5-10%% extra NPS)"
	@echo "  clean   Remove build artifacts"
	@echo "  help    Show this message"
	@echo ""
	@echo "ARCH options:"
	@echo "  x86-64-avx2      Modern CPUs: Intel Haswell+ / AMD Zen+ (default for releases)"
	@echo "  x86-64-bmi2      Intel Haswell+ / AMD Zen 3+ (fastest on most modern PCs)"
	@echo "  x86-64-avx512    Intel Ice Lake+ / AMD Zen 4+ (server/high-end)"
	@echo "  x86-64-popcnt    Legacy 64-bit x86 fallback (broadest compat.)"
	@echo "  apple-silicon    Apple M1/M2/M3/M4 (macOS ARM)"
	@echo "  armv8            Linux ARM64 (Raspberry Pi 5, ARM servers)"
	@echo "  armv8-dotprod    ARMv8.2 with dot-product support (Pi 5, newer ARM servers)"
	@echo "  native           Auto-detect host CPU (for local development only)"
	@echo ""
	@echo "COMP options:"
	@echo "  gcc     Native GCC (default, Linux/macOS)"
	@echo "  mingw   MinGW cross-compile Windows .exe from Linux"
	@echo "  clang   Clang (macOS)"
	@echo ""
	@echo "Examples:"
	@echo "  make build ARCH=x86-64-avx2"
	@echo "  make build ARCH=x86-64-bmi2 COMP=mingw      # Cross-compile Windows EXE"
	@echo "  make pgo   ARCH=x86-64-avx2 COMP=gcc"
	@echo ""
