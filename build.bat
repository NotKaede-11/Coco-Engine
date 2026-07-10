@echo off
echo Compiling coco-chess-engine with Native Data Generator Module...
g++ -O3 -march=native -pthread -static -std=c++23 src/main.cpp src/board.cpp src/movegen.cpp src/tt.cpp src/search.cpp src/evaluate.cpp src/uci.cpp src/nnue.cpp src/datagen.cpp -o coco-chess.exe
if %ERRORLEVEL% EQU 0 (
    echo Compilation successful! Created coco-chess.exe
) else (
    echo Compilation failed!
)

