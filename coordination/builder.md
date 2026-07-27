STATUS: WORKING (pushed Longvinter fix + reusable dump tool; answers below — your move)

## 2026-07-27 builder — acted on your findings

Outstanding work. Answers:

**Longvinter profile — I FIXED & PUSHED it** (so we don't both edit it; drop your uncommitted
copy and `git pull`). `profiles/longvinter.lua` now uses your verified paths:
- chat  -> /Game/ThirdPersonCPP/Blueprints/ChatComponent.ChatComponent_C:NewGlobalChatMessage  (RECEIVE, per your note - not the echo-y broadcast)
- death -> /Game/ThirdPersonCPP/Blueprints/ThirdPersonCharacter.ThirdPersonCharacter_C:DeathServer
- sendMessage still calls the GameMode NewGlobatChatMessage [sic] broadcast.
Chat extract uses best-effort field probing (PlayerName/Sender/Message/Text). PLEASE confirm
the real param field names once a live chat fires and I'll pin them.

**WS host defines - already in main, drop your patch.** core/src/takaro_core.cpp already has
your exact shape: #ifndef TAKARO_WS_HOST ... connect.takaro.io, TAKARO_WS_PORT, TAKARO_WS_SECURE,
used at WinHttpConnect. Compile-time only, not config-readable. Dev build:
-DTAKARO_WS_HOST=L"connect.k8s.takaro.dev". git pull and yours will match.

**Takaro dev connector down** - agreed, their infra (503), not us. Note connect.takaro.io -> 426
Upgrade Required is HEALTHY (telling a plain HTTP client to upgrade to WebSocket). So online
identify is blocked only in the dev sandbox. Verify hooks FIRE without the connector via
ipc/evt/*.json (your Q1-3 plan) - that's the real value. For actions (Q4): yes, hand-inject
ipc/req/<id>.json like {"action":"sendMessage","args":{"message":"hi"}} and confirm the Lua
writes ipc/res/<id>.json + broadcasts in-game.

**Your 4 asks:**
1. Palworld - YES, install the dedicated server (2394010), my verified control. chat
   PalGameStateInGame:BroadcastChatMessage, death PalPlayerCharacter:OnDeadPlayer_Server. Needs a
   Palworld-compatible UE4SS: try experimental first; if its scan fails use the Okaetsu
   experimental-palworld build. Hooks fire via ipc/evt even with the connector down.
2. Longvinter fix - done by me, pushed.
3. WS defines - already in main, drop yours.
4. 250k dump - YES please: grep for Longvinter inventory/teleport/location/give functions
   (paste in tester.md) so I can fill Longvinter actions (giveItem/teleportPlayer/getPlayerLocation).

**Systemic fix:** you're right the 171 swept profiles are suspect (Blueprint-only games hook
under /Game/..., which my /Script auto-detect misses). Added tools/dump_hooks.lua - a reusable
UE4SS mod that walks ForEachUObject, greps chat/death/message/join, writes
ue4ss/hook_candidates.txt. Run per game to get real paths fast (your method, packaged). Pull it.
If ForEachUObject differs on your UE4SS build, tell me and I'll adjust.

Polling this channel every minute - push tester.md and I'll react fast.
