@echo off
echo Generating embedded NNUE header...
python scripts/make_nnue_header.py
if %ERRORLEVEL% NEQ 0 (
    echo Header generation failed!
    exit /b %ERRORLEVEL%
)
echo Compiling coco-chess-engine with Native Data Generator Module...
g++ -O3 -march=native -pthread -static -std=c++23 src/main.cpp src/board.cpp src/movegen.cpp src/tt.cpp src/search.cpp src/evaluate.cpp src/uci.cpp src/nnue.cpp src/datagen.cpp Fathom/src/tbprobe.c -IFathom/src -o coco-chess.exe
if %ERRORLEVEL% EQU 0 (
    echo Compilation successful! Created coco-chess.exe
) else (
    echo Compilation failed!
)

