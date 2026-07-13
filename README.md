# ServerManager-Takaro

**One universal in-game mod that connects any Unreal Engine dedicated server to [Takaro](https://takaro.io/pricing/?via=zach550)** — player join/leave, chat, deaths, and admin control (give item, teleport, kick/ban, inventory, location) from the Takaro dashboard and Discord.

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
events/actions through small files under `ipc/` (atomic write-then-rename).

**Chat/death hooks auto-discover at runtime:** the mod reads the game's real
GameState / PlayerController / Character class paths and probes them for common chat and
death UFunctions — so most games work with no per-game function names at all. Player
join/leave/roster works on any UE game via the engine-standard `FindAllOf("PlayerState")`.

## What was tested (overnight compatibility sweep)

Every Unreal / undetermined-engine game in a 543-title catalog was downloaded, booted
with UE4SS + this mod, and observed. Full breakdown in
[`SWEEP-RESULTS.md`](SWEEP-RESULTS.md) and [`SUPPORTED.md`](SUPPORTED.md):

| Tier | Count | Meaning |
|------|-------|---------|
| **Verified** | 1 | Palworld — full live chat + death hooks confirmed on a running server |
| **Confirmed compatible (mod-ok)** | 53 | UE4SS + the connector load cleanly; hooks auto-discover on a live server with a world + players |
| Unreal, unconfirmed in bare test | 81 | Confirmed Unreal, but injection didn't complete headless (anti-tamper, or the server needs game-specific config to stay running) |
| Not Unreal | 34 | Resolved out of the "undetermined-engine" list |
| Ownership-gated / non-UE | ~370 | See [`SkippedServers.md`](SkippedServers.md) |

> **On verification:** a bare headless test server can only fully verify games that
> auto-load a world on boot (Palworld). Config-heavy survival servers (Soulmask,
> Astroneer, Myth of Empires…) inject UE4SS fine but exit before world-load without their
> real server config — so they show as *confirmed-compatible*, and their hooks
> auto-discover once running on a properly-configured server (i.e. yours).

## Repository layout

```
core/                        the universal winmm.dll (identical for every game)
  src/winmm.cpp              winmm proxy + UE4SS bootstrap + core launcher
  src/takaro_core.cpp        WinHTTP WebSocket + file IPC + Takaro dispatch
  src/json.hpp  src/winmm.def
  build.sh  BUILD.md         MinGW cross-compile (from Linux/WSL) → ~1 MB DLL
mod/TakaroConnector/         the universal UE4SS Lua mod
  Scripts/main.lua           shared core: file IPC, roster diff, hook orchestration
  Scripts/autodetect.lua     runtime chat/death discovery from the game's real classes
  Scripts/json.lua  Scripts/profile.template.lua
  TakaroConfig.txt
profiles/                    per-game profiles (palworld & longvinter hand-verified,
                             plus a profile per swept game)
docs/                        GitHub Pages setup guide (index.html + sitemap + robots)
SWEEP-RESULTS.md             every game by compatibility tier
SUPPORTED.md                 confirmed-compatible list
SkippedServers.md            non-Unreal + ownership-gated games
```

## Build the DLL

```sh
cd core && ./build.sh          # → core/winmm.dll  (MinGW-w64, static, ~1 MB)
```
See [`core/BUILD.md`](core/BUILD.md).

## Install (per game server)

1. Install the game-appropriate UE4SS into `...\Binaries\Win64\`.
2. Drop `winmm.dll` (+ the experimental UE4SS build) and `ue4ss\Mods\TakaroConnector\` in.
3. Set `SERVER_NAME` + `REGISTRATION_TOKEN` in `TakaroConfig.txt` (Takaro → create a
   **Generic** game server, copy its token).
4. Enable `TakaroConnector : 1` in `ue4ss\Mods\mods.txt` and start the server.

## Add a new game

Copy [`mod/TakaroConnector/Scripts/profile.template.lua`](mod/TakaroConnector/Scripts/profile.template.lua)
to `profile.lua` — usually you only need the chat hook + `players()`, and often the
runtime auto-discovery finds them for you. No C++ changes, no DLL rebuild.

## License

MIT
