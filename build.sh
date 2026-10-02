#!/usr/bin/env bash
# Build every stage of the mini database engine.
# Usage:  ./build.sh
set -euo pipefail

FLAGS="-std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wcast-align -Wconversion -Wsign-conversion -Wnull-dereference"

if ! command -v g++ >/dev/null 2>&1; then
    echo "g++ not found on PATH. Install a C++17 compiler (g++/clang++)." >&2
    exit 1
fi

for s in main main_stage2 main_stage3 main_stage3b main_stage4 tests main_repl; do
    echo "=== building $s ==="
    g++ $FLAGS "$s.cpp" -o "$s"
done

echo
echo "All stages built. Run e.g.:  ./main_stage4   (tests: ./tests)"
