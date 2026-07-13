# ServerManager-Takaro

**One universal in-game mod that connects any Unreal Engine dedicated server to [Takaro](https://takaro.io/pricing/?via=zach550)** — player join/leave, chat, deaths, and full admin control (give item, teleport, kick/ban, inventory, location) from the Takaro dashboard and Discord.

The entire Takaro connection lives inside a single self-contained **`winmm.dll`** that loads with your server and holds the secure WebSocket to Takaro **itself**. There is **no bridge process, no Node.js, no client mod**. A tiny [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) Lua *profile* hooks the game's chat/death functions; everything else is shared and identical for every game.

**Setup guide:** https://mad-001.github.io/ServerManager-Takaro/

## How it works

```
Unreal dedicated server (UE4/UE5)                                     Takaro
┌───────────────────────────────┐   ┌──────────────────────────┐   ┌──────────────┐
│ UE4SS + TakaroConnector (Lua)  │   │ winmm.dll (in-process)   │   │ Takaro Cloud │
│  · chat / death hooks          │──▶│  · WinHTTP TLS WebSocket │──▶│ wss://connect│
│  · roster → join/leave/death   │◀──│  · request / response    │◀──│  .takaro.io/ │
│        ▲ files (ipc/)          │   └──────────────────────────┘   └──────────────┘
└───────────────────────────────┘
```

`winmm.dll` proxies the real winmm, bootstraps UE4SS, and runs the Takaro core on a
worker thread. UE4SS Lua has no sockets — so the Lua profile and the DLL exchange
events/actions through small files under `ipc/` (atomic write-then-rename). This is the
same idea VoxelTurf's connector uses over UDP, adapted to UE4SS's file-only Lua sandbox.

## Repository layout

```
core/                        the universal winmm.dll (identical for every game)
  src/winmm.cpp              winmm proxy + UE4SS bootstrap + core launcher
  src/takaro_core.cpp        WinHTTP WebSocket + file IPC + Takaro dispatch
  src/json.hpp  src/winmm.def
  build.sh  BUILD.md         MinGW cross-compile (from Linux/WSL) → ~1 MB DLL
mod/TakaroConnector/         the universal UE4SS Lua mod
  Scripts/main.lua           shared core: file IPC, roster diff, hook orchestration
  Scripts/json.lua  Scripts/autodetect.lua
  Scripts/profile.template.lua   copy → profile.lua per game (~20 lines)
  TakaroConfig.txt
profiles/                    ready-made per-game profiles (palworld.lua, ...)
docs/                        GitHub Pages setup guide (index.html + sitemap + robots)
SkippedServers.md            games out of scope (non-Unreal, or owned-gated dedi server)
```

## Build the DLL

```sh
cd core && ./build.sh          # → core/winmm.dll  (MinGW-w64, static, ~1 MB)
```
See [`core/BUILD.md`](core/BUILD.md).

## Add a new game

Copy [`mod/TakaroConnector/Scripts/profile.template.lua`](mod/TakaroConnector/Scripts/profile.template.lua)
to `profile.lua`, fill in the chat hook + `players()` (usually all you need), and drop it
next to `main.lua`. No C++ changes, no DLL rebuild.

## License

MIT
