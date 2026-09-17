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
  -o ../version.dll version.cpp takaro_core.cpp version.def -lwinhttp -lws2_32 2>/dev/null
ls -la ../winmm.dll ../version.dll

# ── Linux native core (libtakarocore.so) ───────────────────────────────────────────
# The SAME takaro_core.cpp, built for native UE4SS-Linux: our Lua profile runs unchanged
# and this .so (LD_PRELOAD'd into the server process) holds the TLS websocket to Takaro
# via OpenSSL, since WinHTTP has no Linux twin. No Wine/Proton.
# Needs g++ + OpenSSL headers (libssl-dev), or falls back to Node's bundled OpenSSL 3
# headers; links the system libssl.so.3 / libcrypto.so.3 present on any Debian/Ubuntu host.
if command -v g++ >/dev/null 2>&1; then
  OSSL_INC=""
  if [ -f /usr/include/openssl/ssl.h ]; then OSSL_INC="/usr/include"
  else OSSL_INC="$(dirname "$(find "$HOME/.nvm" -path '*include/node/openssl/ssl.h' 2>/dev/null | head -1)")/.." ; fi
  if [ -n "$OSSL_INC" ] && [ -f "$OSSL_INC/openssl/ssl.h" ]; then
    echo "building libtakarocore.so (Linux, openssl inc: $OSSL_INC)..."
    g++ -O2 -std=c++17 -fPIC -shared -I . -I "$OSSL_INC" \
      takaro_core.cpp -o ../libtakarocore.so \
      -l:libssl.so.3 -l:libcrypto.so.3 -lpthread 2>/dev/null && ls -la ../libtakarocore.so
  else
    echo "skip libtakarocore.so: no OpenSSL headers (apt install libssl-dev)"
  fi
fi
