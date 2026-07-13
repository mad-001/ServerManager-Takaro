# Building the universal `winmm.dll` core

`winmm.dll` is a `winmm` DLL proxy that **also contains the entire Takaro core** — no
separate bridge process, no Node. It connects to Takaro over a native WinHTTP secure
WebSocket and to the UE4SS Lua profile over a tiny file-based IPC protocol. It is
**identical for every Unreal Engine game**; the only per-game piece is the Lua profile.

## Files
- `winmm.cpp` — the winmm proxy (forwards every winmm export to the real
  `System32\winmm.dll`), bootstraps UE4SS if present, and starts the core thread.
- `takaro_core.cpp` — the core: config, WinHTTP WebSocket, file IPC, event/roster
  polling, Takaro request dispatch.
- `json.hpp` — vendored [nlohmann/json](https://github.com/nlohmann/json) single header.
- `winmm.def` — the export list for the proxy.

## Build (MinGW-w64, from WSL/Linux)
```sh
./build.sh                 # writes ../winmm.dll
```
or directly:
```sh
x86_64-w64-mingw32-g++ -O2 -std=c++17 -shared \
  -static -static-libgcc -static-libstdc++ \
  -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
  -o winmm.dll winmm.cpp takaro_core.cpp winmm.def \
  -lwinhttp -lws2_32
```

Notes:
- **Do NOT** link `-lwinmm` — this DLL *replaces* winmm; it forwards at runtime via
  `LoadLibrary`/`GetProcAddress`.
- The ~490 `-Wattributes` warnings are expected — the proxy redeclares the winmm
  functions without `dllimport`.
- Static linking → depends only on `KERNEL32`, `WINHTTP`, `msvcrt`. ~1 MB.

## Runtime layout (per game server)
```
<game exe dir>\winmm.dll                                  (this DLL)
<game exe dir>\ue4ss\ ...                                 (UE4SS runtime)
<game exe dir>\ue4ss\Mods\TakaroConnector\TakaroConfig.txt (edit: SERVER_NAME + token)
<game exe dir>\ue4ss\Mods\TakaroConnector\Scripts\main.lua (universal Lua core)
<game exe dir>\ue4ss\Mods\TakaroConnector\Scripts\profile.lua (per-game function names)
<game exe dir>\ue4ss\Mods\TakaroConnector\ipc\            (auto-created: evt/ req/ res/)
```
The DLL reads `TakaroConfig.txt`, writes `core.log`, and exchanges events/actions with
the Lua profile through `ipc/`.

## Config keys (`TakaroConfig.txt`)
```
SERVER_NAME=My Server            # becomes the Takaro identityToken
REGISTRATION_TOKEN=...           # paste from Takaro (Generic game server)
ENABLED=true
POLL_INTERVAL_MS=1000            # how often the core drains events / reads roster
REQ_TIMEOUT_MS=6000              # how long an action waits for the Lua result
SERVER_STARTED_MSG=Server started
```
