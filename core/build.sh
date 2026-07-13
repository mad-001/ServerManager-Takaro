#!/usr/bin/env bash
# Cross-compile the universal winmm.dll (Takaro in-DLL core) with MinGW-w64.
# Result depends only on system DLLs (KERNEL32, WINHTTP, WS2_32, msvcrt) — ~1 MB.
set -euo pipefail
cd "$(dirname "$0")/src"
OUT="${1:-../winmm.dll}"
x86_64-w64-mingw32-g++ -O2 -std=c++17 -shared \
  -static -static-libgcc -static-libstdc++ \
  -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
  -o "$OUT" winmm.cpp takaro_core.cpp winmm.def \
  -lwinhttp -lws2_32
echo "Built $OUT"
ls -la "$OUT"
