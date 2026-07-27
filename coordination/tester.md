STATUS: WORKING

## 2026-07-27 00:04 UTC tester — action candidates for Longvinter + two corrections

Pulled your fixes. WS defines match mine exactly, so I dropped my patch. Longvinter
profile looks right — note you moved `chat.candidates` to a single `chat.hook`; the
runtime resolves it fine and both hooks register.

### CORRECTION: Palworld is already installed — on the MAIN PC, not the rig

You said "install the dedicated server". No need — **your** test server is where you left it:
```
C:\Program Files (x86)\Steam\steamapps\common\TakaroTestServers\palworld
  PalServer.exe · Pal/Binaries/Win64/winmm.dll · ue4ss/Mods/TakaroConnector/   (+ slotA, slotB)
```
The Palworld **client** is on that machine too (`steamapps/common/Palworld`), so server
and client can both run there. My earlier "not present" was me checking the *laptop* rig —
a different machine. Nothing to install.

Its `core.log` from 2026-07-13 is the most useful artifact in the repo right now:
```
Connected. Identifying as "TakaroTest-Palworld"
Takaro confirmed connection
Identify failed: {"http":400,"message":"Invalid registrationToken provided"}
```
That proves the **entire transport works** — socket, TLS, framing, identify sent, real
Takaro error parsed back. Only the token was stale. So Q5 is largely answered: the identify
path functions and has simply never had a valid token.

### Domain decision: we are NOT touching production

Mad's call, and I agree. The dev **API** is healthy (I use it constantly — login, domains,
tokens, reachability). Only the dev **connector** is down. We are not routing around that
via `connect.takaro.io` into a live community domain. Identify stays blocked until
Catalysm restarts the dev connector; everything else proceeds via `ipc/evt`.

### Q4 — Longvinter action candidates (fresh dump, 250,807 objects, 1,764 matches)

My first dump filtered to chat/death only, so actions were never captured. Re-ran with
inventory/teleport/location/give/kick/ban patterns.

**Give items.** Longvinter's convention is `X` = client entry, `XServer` = server RPC.
On a dedicated server you want the **Server** variants:
```
/Game/Blueprints/PC_Longvinter.PC_Longvinter_C:AdminGiveItemsServer      <- best candidate
/Game/Blueprints/PC_Longvinter.PC_Longvinter_C:AddAdminItemsServer
/Game/Blueprints/PC_Longvinter.PC_Longvinter_C:AdminGiveItems            (client entry)
/Game/Blueprints/PC_Longvinter.PC_Longvinter_C:AddAdminItems             (client entry)
```

**Teleport**
```
/Game/ThirdPersonCPP/Blueprints/ThirdPersonCharacter.ThirdPersonCharacter_C:ContinueTeleport
/Game/ThirdPersonCPP/Blueprints/ThirdPersonCharacter.ThirdPersonCharacter_C:EndTeleport
/Game/Blueprints/PC_Longvinter.PC_Longvinter_C:Client_SendTeleportAdress   [sic]
```
No one-shot `SetActorLocation` wrapper surfaced — teleport looks like a multi-step flow
(`ContinueTeleport` -> `EndTeleport`), so it may need the raw `SetActorLocation` on the
character pawn instead. Say the word and I'll grep for that specifically.

**Location / respawn**
```
/Game/Blueprints/PC_Longvinter.PC_Longvinter_C:GetRespawnLocation
/Game/Blueprints/PC_Longvinter.PC_Longvinter_C:SetInitialLocation
/Game/Blueprints/PC_Longvinter.PC_Longvinter_C:Respawn
/Game/Blueprints/Server/GM_Longvinter.GM_Longvinter_C:Get Random Spawn Location
```

**Admin surface on the GameMode** — confirms your existing kick/ban, and note the path is
`/Game/Blueprints/Server/`, not `/GameModes/`:
```
GM_Longvinter_C:BanBySteamID · BanPlayer · CheckServerRestart
GM_Longvinter_C:AddMessageToPlayerSave · AddStringsToBannedWords
```

Full dump is on the rig at `C:\GameServers\ufunc-actions.txt`. Tell me what else to grep.

### Next from me
Booting your Palworld server on the main PC, joining with the real client, and answering
Q1-Q3 from `ipc/evt/*.json` with the connector down. Then hand-injecting `ipc/req` for Q4.

---


## 2026-07-26 23:55 UTC tester — rig is live; blocked on infra, plus one confirmed profile bug

### Read this first: two of your assumptions don't hold on my machine

**1. There is no Palworld test server here.**
`C:\Program Files (x86)\Steam\steamapps\common\TakaroTestServers\` does not exist on
the test rig. That path must be on the machine the previous session used. This rig is a
separate Windows laptop (Laptop4060, 192.168.1.25) driven remotely over WinRM + a desk
agent. Its whole Steam library is:
```
Longvinter · Steam Controller Configs · Steamworks Shared
```
Mad owns Palworld, so I can install the Palworld **dedicated server** (appid 2394010) here
if you want — say the word and I'll do it. Meanwhile I used **Longvinter** (server appid
1639880, free/anonymous), which is what's installed.

**2. The Takaro dev connector is DOWN — this blocks your Q1, Q4 and Q5 entirely.**
```
connect.k8s.takaro.dev   -> HTTP 503 "no healthy upstream"
connect.takaro.io (prod) -> HTTP 426 Upgrade Required   (healthy, for contrast)
```
Not our config. Every GENERIC gameserver in the Koji sandbox reports
`connectable: false / "No response received"`, **including Catalysm's own `harambe-*`
lanes**. The one server that works is `Iron Mesa 7D2D` — type SEVENDAYSTODIE, which
doesn't use the connector at all. So identify cannot succeed for anyone right now.

Also: `next.takaro.dev` is fully dead (522 on both api and connect) if anything still
points there.

The token is NOT the blocker any more — I pulled a current one live from
`GET /me` → `serverRegistrationToken` on `api.k8s.takaro.dev` (domain
`proud-rats-train`, the Koji sandbox). The dev **API** is fine; only the connector is down.

---

### CONFIRMED BUG: the shipped Longvinter profile never resolves its hooks

This is the important finding. `profiles/longvinter.lua` (and
`Longvinter/TakaroConnector/Scripts/profile.lua`) ships three chat candidates and **all
three are wrong**, so chat and death silently do nothing:

```
[Lua] [Takaro] none of 3 candidate hooks resolved
[Lua] [Takaro] auto-detect: no chat UFunction resolved — dump UFunctions and set profile.chat.hook
[Lua] [Takaro] Chat: no hook resolved (roster/join/leave still work)
[Lua] [Takaro] Death: no spec (using roster death-count diff if available)
```

Cause, from a live dump of **250,871 UObjects** on a running 1.7 GB dedicated server:

| profile said | reality |
|---|---|
| `/Game/Blueprints/GameModes/GM_Longvinter.GM_Longvinter_C:NewGlobatChatMessage` | it's under `/Game/Blueprints/**Server**/`, not `GameModes` |
| `/Script/Longvinter.LongvinterGameState:BroadcastChatMessage` | **no `/Script/Longvinter.*` classes exist** — the game is Blueprint-only |
| `/Script/Longvinter.LongvinterPlayerController:ClientReceiveChatMessage` | same — doesn't exist |

Real paths (verified):
```
chat receive : /Game/ThirdPersonCPP/Blueprints/ChatComponent.ChatComponent_C:NewGlobalChatMessage
team chat    : /Game/ThirdPersonCPP/Blueprints/ChatComponent.ChatComponent_C:NewTeamChatMessage
chat send    : /Game/Blueprints/Server/GM_Longvinter.GM_Longvinter_C:NewGlobatChatMessage   [sic]
death        : /Game/ThirdPersonCPP/Blueprints/ThirdPersonCharacter.ThirdPersonCharacter_C:DeathServer
```
Note the two spellings are genuinely different functions: the GameMode's misspelled
`NewGlobatChatMessage` is the server **broadcast** (what `actions.sendMessage` calls);
`ChatComponent_C:NewGlobalChatMessage` (correct spelling) is the per-player **receive**.
Hooking only the former would echo Takaro's own messages back.

After fixing the profile, both hooks register:
```
[Lua] [Takaro] hook resolved -> /Game/ThirdPersonCPP/Blueprints/ChatComponent.ChatComponent_C:NewGlobalChatMessage
[RegisterHook] Registered script hook (5, 5) for Function .../ChatComponent_C:NewGlobalChatMessage
[Lua] [Takaro] Chat hook: .../ChatComponent.ChatComponent_C:NewGlobalChatMessage
[Lua] [Takaro] hook resolved -> /Game/ThirdPersonCPP/Blueprints/ThirdPersonCharacter.ThirdPersonCharacter_C:DeathServer
[RegisterHook] Registered script hook (6, 6) for Function .../ThirdPersonCharacter_C:DeathServer
[Lua] [Takaro] Death hook: .../ThirdPersonCharacter_C:DeathServer
```

**Implication for the other 171 profiles:** the sweep matched patterns without a running
game. The first one checked against a live server was wrong, and wrong in a way that reads
as working. I'd treat every unverified profile as suspect until dumped.

Method that works (same one you used for Palworld's `OnDeadPlayer_Server`): drop a UE4SS
Lua mod that walks `ForEachUObject` and greps `GetFullName()` for chat/death/message,
write to a file, read it back.

---

### Two gaps in the shipped release

**UE4SS is not in the zip.** v1.1.1 ships `winmm.dll`, `version.dll`,
`ue4ss/Mods/TakaroConnector/`, `ue4ss/Mods/mods.txt`, `profiles/` — but **not**
`UE4SS.dll`, `UE4SS-settings.ini` or `UE4SS_Signatures`. `winmm.cpp` says
"bootstraps UE4SS **if present**". The docs page does say to install UE4SS first, but the
failure mode is silent: server starts, nothing happens, no `core.log`. I had to copy the
UE4SS runtime off Mad's production Longvinter server to make the test rig work.
An `install.bat` (detect game → fetch matching UE4SS → lay out tree → copy
`profiles/<game>.lua` → `profile.lua` → write `mods.txt` → verify) would kill this
whole class of failure. There is no installer in the repo today.

**The websocket host is hardcoded to production**, with no config key —
`takaro_core.cpp` had `WinHttpConnect(..., L"connect.takaro.io", 443, ...)` and
`WINHTTP_FLAG_SECURE`. So the mod could not be pointed at staging/dev at all.
I patched it (uncommitted, my working tree) to compile-time defines only:
```c
#ifndef TAKARO_WS_HOST    #define TAKARO_WS_HOST   L"connect.takaro.io"  #endif
#ifndef TAKARO_WS_PORT    #define TAKARO_WS_PORT   443                   #endif
#ifndef TAKARO_WS_SECURE  #define TAKARO_WS_SECURE 1                     #endif
```
Deliberately **not** readable from `TakaroConfig.txt`, per Mad — a shipped DLL must never
be redirectable by an end user. Verified: default build still resolves
`connect.takaro.io`; a `-DTAKARO_WS_HOST=L"connect.k8s.takaro.dev"` build resolves the
dev host. Tell me if you want that shape or your own and I'll drop mine.

---

### Your 5 questions — current answers

1. **player-connected reaches Takaro?** — BLOCKED. Connector down, so nothing reaches
   Takaro. But I can verify the hook *fires* without Takaro: `TC.emit()` writes
   `ipc/evt/*.json` independently of the socket. Doing that next.
2. **chat-message fires with correct Sender/Message?** — NOT YET. Hook now registers on
   Longvinter (it didn't before). Client is launched and at the main menu; joining next.
3. **player-death via OnDeadPlayer_Server?** — N/A for Longvinter; equivalent is
   `ThirdPersonCharacter_C:DeathServer`, now registered, not yet fired.
4. **Takaro→game actions?** — BLOCKED. Requests arrive over the websocket
   (`ipc/req/*.json`), so nothing to round-trip while the connector is down. I can inject
   a `req` file by hand to test the Lua half in isolation if useful — say if you want that.
5. **Which token / did identify succeed?** — Current valid Koji token from `GET /me`.
   Identify never got a chance: `core.log` shows the socket failing before handshake —
```
   ServerManager-Takaro core (in-DLL) started — mod=C:\GameServers\longvinter-test\...\TakaroConnector
   Disconnected. Reconnecting in 3s
   Disconnected. Reconnecting in 6s
   Disconnected. Reconnecting in 12s   (backoff to 60s, repeating)
```
   No "Connected. Identifying" line at all — it never opens.

---

### What I need from you

1. **Palworld**: want me to install the Palworld dedicated server on this rig? It's the
   one game you've verified, so it's the best control. I'll do it unless you object.
2. **Longvinter profile fix** — do you want to take my paths and push it, or shall I? I'm
   holding it uncommitted to avoid us both editing the same file.
3. **Compile-time WS defines** — keep mine, or you do it your way?
4. Anything you want from the 250k UObject dump while I have the server up — I can grep it
   for inventory/teleport/location functions to fill in Longvinter's `actions`.

### Rig capabilities, so you know what I can drive
Remote Windows laptop: WinRM shell, SMB filesystem, screenshots, scancode keyboard/mouse
input (`SendInput` w/ `KEYEVENTF_SCANCODE` — virtual-key input silently fails in
DirectX games), process launch. I can install servers via SteamCMD, boot them, launch the
game client, and act as a real player.
