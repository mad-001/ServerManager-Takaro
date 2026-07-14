#!/usr/bin/env bash
# Cross-compile the universal proxy DLLs (Takaro in-DLL core) with MinGW-w64.
# Ship whichever proxy your game imports (winmm / version); UE4SS's dwmapi also works.
# The core has a single-instance guard, so shipping multiple proxies together is safe.
set -euo pipefail
cd "$(dirname "$0")/src"
echo "building winmm.dll..."
x86_64-w64-mingw32-g++ -O2 -std=c++17 -shared -static -static-libgcc -static-libstdc++ \
  -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
  -o ../winmm.dll winmm.cpp takaro_core.cpp winmm.def -lwinhttp -lws2_32 2>/dev/null
echo "building version.dll..."
x86_64-w64-mingw32-g++ -O2 -std=c++17 -shared -static -static-libgcc -static-libstdc++ \
  -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
  -o ../version.dll version.cpp takaro_core.cpp version.def -lwinhttp 2>/dev/null
ls -la ../winmm.dll ../version.dll
