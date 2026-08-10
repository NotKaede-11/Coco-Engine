@echo off
echo Generating embedded NNUE header...
python scripts/make_nnue_header.py
if %ERRORLEVEL% NEQ 0 (
    echo Header generation failed!
    exit /b %ERRORLEVEL%
)
python scripts/make_build_info.py --arch native --compiler gcc
if %ERRORLEVEL% NEQ 0 (
    echo Build identity generation failed!
    exit /b %ERRORLEVEL%
)
echo Compiling coco-chess-engine with Native Data Generator Module...
g++ -O3 -march=native -pthread -static -std=c++20 -DL1_SIZE=512 -DNDEBUG src/main.cpp src/board.cpp src/movegen.cpp src/tt.cpp src/search.cpp src/evaluate.cpp src/uci.cpp src/nnue.cpp src/datagen.cpp Fathom/src/tbprobe.c -IFathom/src -o coco-chess.exe
if errorlevel 1 (
    echo Compilation failed!
    exit /b 1
)
echo Compilation successful! Created coco-chess.exe
