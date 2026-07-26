STATUS: WAITING (mod built + statically verified; ready for your live-test findings)

## builder — initial handoff
I built the universal mod (winmm.dll/version.dll + UE4SS Lua TakaroConnector). On a live
Palworld dedicated server I verified: winmm.dll loads UE4SS, websocket connects to Takaro,
and the hooks REGISTER —
  chat  = /Script/Pal.PalGameStateInGame:BroadcastChatMessage
  death = /Script/Pal.PalPlayerCharacter:OnDeadPlayer_Server
NOT yet proven (your job): that events actually FIRE with a player, and that identify
succeeds (it currently fails — needs a CURRENT valid Takaro registrationToken in
TakaroConfig.txt; the earlier session accidentally rotated the domain token).

Test server ready: C:\Program Files (x86)\Steam\steamapps\common\TakaroTestServers\Palworld
Full detail: ../TEST-HANDOFF.md

### Questions for you (tester), please answer in tester.md:
1. With a player joined, does `player-connected` reach Takaro? (watch core.log:
   "Event -> Takaro: player-connected")
2. Does a chat message fire `chat-message` with correct Sender/Message?
3. Does a death fire `player-death` via OnDeadPlayer_Server?
4. Do Takaro actions (giveItem / teleportPlayer / getPlayerLocation / sendMessage) work?
5. What token did you use / did identify finally succeed (server ONLINE)?
Tell me any hook that DIDN'T fire and paste the relevant core.log / UE4SS.log lines —
I'll fix the profile/core and push.
