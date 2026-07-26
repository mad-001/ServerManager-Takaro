# Test Handoff — ServerManager-Takaro mod status & what to verify

For the session driving a (fake) player to test hooks. This says exactly what's already
proven, what is NOT yet proven, and what you need to do.

## What the mod is
One `winmm.dll` (also `version.dll`; UE4SS's `dwmapi.dll` works too) that:
1. proxies the real DLL, bootstraps UE4SS, and
2. runs an in-process WinHTTP secure WebSocket to `wss://connect.takaro.io/` (no bridge).
A UE4SS Lua mod (`TakaroConnector`) hooks the game and exchanges events/actions with the
DLL over files (`ue4ss/Mods/TakaroConnector/ipc/`). UE4SS Lua has no sockets, hence files.

## TEST STATUS — be precise

### ✅ PROVEN (live, on a real Palworld dedicated server)
- `winmm.dll` loads UE4SS **standalone** (no dwmapi needed).
- WebSocket **connects + TLS + `identify` is sent**: core.log shows `Connected. Identifying`
  then `Takaro confirmed connection`.
- Lua mod loads: `ServerManager-Takaro connector loaded, profile: Palworld` → `connector ready`.
- **Chat hook REGISTERS**: `/Script/Pal.PalGameStateInGame:BroadcastChatMessage`
  (`RegisterHook` returns a native hook id).
- **Death hook REGISTERS**: `/Script/Pal.PalPlayerCharacter:OnDeadPlayer_Server`
  (found via live UFunction dump — the old `OnDeath` does NOT exist on current builds).
- Roster file-IPC writes `ipc/players.json` (`[]` with no players — correct).
- On 53 other UE games: UE4SS + connector load cleanly (compatible), hooks NOT yet fired.

### ❌ NOT PROVEN (this is your job — needs a connected player)
1. **`identify` SUCCESS / server ONLINE in Takaro.** It currently FAILS:
   `Invalid registrationToken`. The previous session rotated the domain registration
   token by mistake. **You must put a CURRENT valid registration token in
   `TakaroConfig.txt` → `REGISTRATION_TOKEN=`** (Takaro dashboard → the Generic game
   server's token). Until then the server won't show online and no events reach Takaro.
2. **Does `chat-message` actually FIRE** when a player types? The hook registers; nobody
   has typed. Confirm the hook fires AND that `Sender`/`Message` come through as strings.
3. **`player-connected` / `player-disconnected`** — driven by roster diff (`players()` every
   ~3s). Confirm a join emits `player-connected` and a leave emits `player-disconnected`.
4. **`player-death`** — confirm `OnDeadPlayer_Server` fires on a real death and emits
   `player-death` with the right player.
5. **Takaro → game ACTIONS** (fully unverified): `giveItem`, `teleportPlayer`,
   `getPlayerLocation`, `sendMessage`. These round-trip via `ipc/req/*.json` →
   Lua executes → `ipc/res/*.json`. Confirm each works.
6. **`gameId` / `steamId`** — Palworld profile currently uses the player NAME as `gameId`
   and sends no `steamId`. If Takaro needs a stable id / steam id, pull it from
   `PalPlayerState` (look for a UID/PlayerUId field) and update `players()` + hook extracts.

## How to test (Palworld — already set up)
Install: `C:\Program Files (x86)\Steam\steamapps\common\TakaroTestServers\Palworld`
(has `winmm.dll`, `ue4ss\`, and `ue4ss\Mods\TakaroConnector\`).
1. Put a valid `REGISTRATION_TOKEN` in
   `Pal\Binaries\Win64\ue4ss\Mods\TakaroConnector\TakaroConfig.txt`.
2. Boot: `PalServer.exe -port=8211 -players=4 -NoAsyncLoadingThread -UseMultithreadForDS -log`
3. Join with a player (your fake-player setup). Watch:
   - `Pal\Binaries\Win64\ue4ss\UE4SS.log` → `[Takaro]` lines + hook fires
   - `...\TakaroConnector\core.log` → `Identified with Takaro`, `Event -> Takaro: <type>`
   - `...\TakaroConnector\ipc\evt\*.json` → queued events (Lua→DLL), and `players.json`
4. In Takaro, trigger actions (giveItem/teleport/sendMessage) and confirm in-game.

## Event wire format (what the DLL sends Takaro)
`{"type":"gameEvent","payload":{"type":"<t>","data":{"type":"<t>", ...}}}`
Types: `player-connected`, `player-disconnected`, `chat-message` (`{player,msg,channel}`),
`player-death`, `log`. Player object: `{name, gameId, steamId?}`. Full spec in the
repo's `Websocket Connection Instructions.md` reference.

## What still needs doing (summary)
- [ ] Valid registration token → identify success → server online (BLOCKER, needs the token).
- [ ] Confirm chat / join / leave / death events actually fire & reach Takaro (with a player).
- [ ] Confirm Takaro→game actions (giveItem/teleport/getLocation/sendMessage) round-trip.
- [ ] Decide gameId/steamId source for Palworld (name vs PlayerUId/steam id).
- [ ] Repeat for the other games you run (Soulmask/Longvinter/MythOfEmpires/Astroneer) on
      their REAL configured servers — bare test servers exit before world-load.
- [ ] The 53 `mod_ok` games: hooks auto-discover at runtime; confirm per game as needed.

Repo: github.com/mad-001/ServerManager-Takaro · releases through v1.1.1 · SWEEP-RESULTS.md,
SUPPORTED.md, SkippedServers.md have the full classification.
