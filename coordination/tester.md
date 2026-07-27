STATUS: WORKING

## 2026-07-27 01:53 UTC tester — ✅ ECHO-GUARD CONFIRMED WORKING. Real-player chat format in progress.

Deployed your Lua-only fixes (`main.lua` + `profiles/longvinter.lua` -> `profile.lua`),
kept the existing DLL as you said, restarted. GM hook re-registered cleanly.

### ✅ BUG 1 FIXED — verified, not assumed
```
inject  {"id":"g1","action":"sendMessage","args":{"message":"echo guard probe"}}
   ->   res.json = {"success":true,"id":"g1","result":"broadcast"}
   ->   ipc/evt/  = 0 files
```
Broadcast succeeded **and produced zero events**. Before your fix the identical test emitted
a `chat-message` containing our own text. The recordSend/isEcho containment window works —
the feedback loop is closed.

### Still to confirm: BUG 2 (real-player sender format)
Needs a human-typed message, which I'm setting up now. I'll report the **exact raw broadcast
string** so you can pin the format rather than keep the provisional `"Name: message"` split.

### 🔧 Rig lesson worth recording for the skill
The agent lost screen capture with:
```
Exception calling "CopyFromScreen": "The handle is invalid"
```
Not a lock screen (`LogonUI` absent) — the agent had lost its **desktop handle** after the
interactive session disconnected/reconnected (RustDesk closing). `/health` still answered
200 the whole time, so liveness checks don't catch it. **Fix: restart the agent**; it
reattaches immediately. Adding "restart the agent after any remote-session disconnect" to
the rig docs. Relevant to you only in that a green `/health` doesn't prove the agent can
still see or drive the screen.

### Scoreboard
| | |
|---|---|
| Q1 player-connected | ✅ PROVEN, reproducible |
| Q2 chat hook fires | ✅ PROVEN (GM broadcast, via our own sendMessage) |
| Q4 inbound actions | ✅ PROVEN (mailbox, repeatable across ids) |
| BUG 1 echo-guard | ✅ FIXED + VERIFIED |
| BUG 2 sender extract | ⏳ needs one real player message |
| Q3 death | ⏳ needs a player |
| Q6 steamId | ⏳ needs a player |
| Q5 identify | ⛔ dev connector still 503 |

Four greens. The remaining three all need the same thing: a live player, which I'm bringing
up now.

---


## 2026-07-27 01:39 UTC tester — ✅ MAILBOX WORKS. Q4 green, Q2 green. Two new bugs in the chat extract.

Your fix landed perfectly. Deployed `winmm.dll` (rebuilt from your source with
`-DTAKARO_WS_HOST=L"connect.k8s.takaro.dev"` so my rig stays off production — your default
prod build is untouched) + `main.lua`, wiped the old `req`/`res` dirs, restarted.

### ✅ Q4: INBOUND IS ALIVE — first ever green
```
inject  ipc/req.json = {"id":"t1","action":"sendMessage","args":{"message":"probe"}}
        -> req.json DELETED by the mod
        -> ipc/res.json = {"success":true,"result":"broadcast","id":"t1"}
repeat  id "t2"
        -> ipc/res.json = {"success":true,"result":"broadcast","id":"t2"}
```
Correct id echoed both times, consumed within the poll window, repeatable. The single-file
mailbox + `io.open` was the right call. **Takaro→game actions now work.**

### ✅ Q2: THE CHAT HOOK FIRES — and it's the GameMode broadcast
Our own `sendMessage` broadcast tripped the hook, producing a real event:
```json
{"type":"chat-message","data":{"channel":"global","msg":"[Takaro] probe",
 "player":{"gameId":"[Takaro] probe","name":"[Takaro] probe"}}}
```
So `/Game/Blueprints/Server/GM_Longvinter.GM_Longvinter_C:NewGlobatChatMessage` **does
execute server-side and the hook does fire.** My earlier client-side hypothesis is now
supported by evidence, not just reasoning. Pin the GM broadcast for Longvinter.

### 🐛 BUG 1 — the [Takaro] echo-guard did NOT engage
The event above IS our own message. `msg` is literally `"[Takaro] probe"`, so the guard
that should skip our `[Takaro] ` prefix didn't fire. As shipped, every message Takaro
sends will bounce straight back as an inbound `chat-message` — a feedback loop the moment
a module echoes chat. Worth checking whether the guard is in the version I deployed
(I pulled `main.lua` at `b833521`) or only in your working tree.

### 🐛 BUG 2 — sender extraction is wrong; the broadcast has no sender
`gameId` and `name` both came back as **the message text**, not a player. That's the
generic field probe finding nothing and falling through to the message.

The real issue is structural: `NewGlobatChatMessage` looks like a **single-FString
broadcast** — there is no sender parameter to extract. So for Takaro's own messages there's
genuinely no player, and `player` should probably be null/server rather than garbage.

**Open question I can't answer yet:** when a REAL player chats, does the server call this
same broadcast with a pre-formatted `"Name: message"` string? If so the extract should
split on the first `": "`. I can't confirm without a real player, and the game client is
off-limits right now (Mad is using the machine). **Queued for the moment I'm clear.**

Suggestion meanwhile: make the extract defensive — if the param has no sender field and
matches `^(.-):%s(.*)$`, treat group 1 as name and group 2 as msg; otherwise emit the
message with a null/server player rather than duplicating the text into both fields.

### Deploy note for you
Your prod `core/winmm.dll` at `b833521` is fine and I did NOT modify it. I only built a
dev-host variant locally for the rig (not committed, per ROLES.md).

### Next from me, no client needed
- Q3 death — can't fake a death without a player; queued
- Q6 steamId — queued, needs a live player
- RCON track — Longvinter exposes no RCON in the UFunction dump, so `RCON_PORT=0` there;
  happy to exercise it on a game that has one when we get to it

### Unchanged
Q1 PROVEN. Q5 identify still parked — dev connector still 503.

---


## 2026-07-27 01:32 UTC tester — paused briefly; saw your mailbox fix land

Mad needs the machine for something local, so the Longvinter server/client are hands-off
for now. **Not a failure — just idle.** I'll resume and retest the moment he's clear.

Saw your fix in `core/src/takaro_core.cpp`:
```c
g_reqFile = g_ipcDir + "\\req.json";   // single-file mailbox (dir listing is dead in UE4SS Lua)
g_resFile = g_ipcDir + "\\res.json";
std::mutex g_actionMutex;               // serializes gameAction
```
That's exactly the shape I hoped for — io.open only, no directory enumeration. Good call
serializing with the mutex too, since a single mailbox can't hold concurrent requests.

### What I'll run the moment I'm back, in this order
1. **Retest the mailbox** — inject `ipc/req.json` = `{"action":"sendMessage","args":{"message":"probe"}}`,
   confirm the Lua consumes it and writes `ipc/res.json`. That's Q4's first real answer.
2. **Q2 chat via the same path** — if `sendMessage` broadcasts, watch whether the chat hook
   fires on our own broadcast. Confirms which hook works AND validates your `[Takaro]`
   echo-guard, with zero client UI involved.
3. **Q3 death**, then **Q6 steamId** property dump.

Two things I need from you before I retest:
- Does the new DLL need rebuilding for the mailbox, or is it Lua-side only? If the DLL
  changed, I need the rebuilt `winmm.dll` (you own builds) — tell me where to pull it from
  and I'll deploy it to the rig.
- Same question for `main.lua` — I'll pull whatever's in the repo, just confirm it's pushed.

### Note on the RCON addition
Saw `RCON_HOST/PORT/PASSWORD` land in the config. Sensible that it's config-readable while
the WS host stays hardcoded — the reasoning in your comment is right (operator's own
localhost, not a security boundary). Longvinter has no RCON as far as I can tell from the
UFunction dump, so it'll stay `RCON_PORT=0` there and fall back to the UE console path.
Happy to test the RCON route on a game that has one — 7D2D or Rust would be the candidates,
though those are natively supported so lower priority.

### Unchanged
Q1 PROVEN. Q5 identify still parked on the dev connector (still 503 last I checked).

---


## 2026-07-27 01:06 UTC tester — 🚨 CRITICAL: ipc/req is NEVER drained. ALL Takaro→game actions are dead.

This is bigger than chat. **`listDir()` in main.lua returns nothing on this UE4SS build**,
so the action poll loop never sees a request. Every action — giveItem, teleportPlayer,
getPlayerLocation, sendMessage, kick, ban, and your new shutdown/executeCommand — can
never execute. The mod can report events TO Takaro but cannot receive anything FROM it.

### Evidence (decisive, not inference)

1. Injected `ipc/req/test1785114208.json` = `{"action":"sendMessage","args":{"message":"..."}}`
2. Waited 10s (poll is 250ms). **Not consumed.** No `ipc/res`. No event. No log line.
3. Confirmed the laptop itself sees the file — `69 bytes`, correct dir, correct name:
   ```
   Name                Length LastWriteTime
   test1785114208.json     69 7/26/2026 8:03:28 PM
   ```
4. No `req poll error` in UE4SS.log — so `pcall` isn't catching an exception; the loop
   simply iterates an **empty list**.
5. **The clincher:** main.lua line ~222 clears stale req/res at startup using the same
   `listDir`. I restarted the server with that file present — **it survived the restart.**
   If `listDir` worked, startup would have deleted it.

### Root cause

```lua
local function listDir(path)
    local h = io.popen('cmd /c "dir /B \\"' .. winPath(path) .. '\\" 2>nul"')   -- <-- returns nothing
```

`io.popen` appears unavailable/no-op in this UE4SS Lua build. Consistent with what else
works:
- `writeAtomic` / `readFile` use **`io.open`** -> WORK (players.json is written every cycle,
  ipc/evt files are written correctly)
- `listDir` uses **`io.popen`** -> FAILS silently, returns `{}`

So it's specifically the popen path. Note the old Longvinter `TakaroAgent` used this same
`io.popen` + `dir /B` trick — worth checking whether that ever actually worked, or whether
the external bridge only ever exercised the write side.

### Why nothing looked broken until now
Everything that has ever been "verified" is **outbound** (hooks -> emit -> ipc/evt -> DLL).
The inbound path has no test that doesn't require a live server, so it shipped untested.
This is exactly the class of bug the rig exists to find.

### Suggested fix — yours to implement (ROLES.md), but the shape

Avoid directory enumeration entirely. Options, cheapest first:

1. **Single-file mailbox.** DLL writes `ipc/req.json` (one pending request, or a JSON array);
   Lua reads it with `io.open`, processes, deletes/truncates, writes `ipc/res.json`.
   Uses only the io.open path that's already proven working. Smallest change.
2. **Index file.** DLL also writes `ipc/req/_index` listing pending ids; Lua reads that with
   `io.open` instead of listing the dir. Keeps the multi-file protocol.
3. **UE4SS native FS API** if the build exposes one — but (1) needs no UE4SS features at all,
   so it's the safest across the 172 games.

I'd take (1) — the multi-file design exists only because directory listing was assumed to
work, and it doesn't.

Tell me when it's pushed and I'll retest immediately with an injected request; I have a
live server + connected player standing by.

### Chat (Q2) — still not confirmed, and now lower priority
GM broadcast hook registers. Typing works, submitting is unreliable via synthetic input
(Mad confirmed Enter works when a human presses it over RustDesk, so it's my input path,
not the game). Not chasing it further until the req fix lands, because:
**if you fix ipc/req, I can test chat by injecting `sendMessage` and watching whether the
hook fires on our own broadcast — no client UI involved at all.** That kills two birds and
removes the flakiest part of my rig from the loop.

### Unchanged
- Q1 player-connected: PROVEN, reproducible
- Q3 death / Q6 steamId: queued
- Q5 identify: parked on the dev connector

---


## 2026-07-27 00:31 UTC tester — DO NOT push the chat fix yet; my earlier "no fire" result was unsound

### Retract: my chat-hook conclusion was based on a broken rig

I found a bug **in my agent**, not the mod, that invalidates the chat test I reported:

```
Method invocation failed because [System.Int32] does not contain a method named 'ContainsKey'
```

PowerShell variables are **case-insensitive**. My handler did `$vk = Resolve-Key ...`,
which silently overwrote the `$VK` keycode **hashtable** with an integer. Net effect: the
**first** keypress after each agent restart worked, and every subsequent one threw.

That is exactly the pattern I saw and misread as a game/hook problem — Enter opened chat
once, then "stopped working". So when I reported *"chat hook registers but never fires"*,
I cannot actually prove the message was ever sent. **Treat Q2 as UNTESTED, not failed.**

Fixed (renamed to `$vkCode` / `$KEYMAP`); three consecutive keypresses now succeed.

### What IS solid

- **Q1 player-connected still stands** — that fired from a real join and is reproducible
  across three separate joins tonight. Unaffected by the input bug.
- **Keyboard input genuinely reaches the game.** Clean test post-fix: held `w` for 3s,
  centre-region pixel delta 9.7M — the character walked. So scancode `SendInput` works
  in-game, which also validates the whole `KEYEVENTF_SCANCODE` approach for every game.
- **DPI fix holds** — clicks land where the screenshot says.

### Where I'm stuck (rig problem, not mod problem)
Enter opens Longvinter's chat box **intermittently**. When it opens, typing works
(I have a screenshot of `takaro hook test 123` sitting in the field). Submitting is the
unreliable part. So I still cannot say whether the hook fires.

**So: keep holding the chat push.** My analysis that the ChatComponent receive is
client-side is still *plausible reasoning*, but I no longer have evidence for it, and I'd
rather you not ship a profile change on my say-so when my instrument was faulty.

I'll get a message actually sent and confirm which hook fires before you change anything.

### Standing asks unchanged
- steamId property dump — still queued, needs a stable in-game session
- Q3 death / Q4 injected ipc/req — after chat

### ROLES.md — agreed
Sensible split. My profile/core edits have all been in the local test tree only; the repo
copies are yours. I'll keep reporting rather than committing to `core/`, `mod/`, `profiles/`.

---


## 2026-07-27 00:21 UTC tester — Q1 PROVEN. Chat hook does NOT fire — suspect it is client-side.

### ✅ Q1: player-connected FIRES — first live proof
Real client, real join, on a real dedicated server:
```json
ipc/evt/00000000000000000001.json
{"type":"player-connected","data":{"player":{"gameId":"Mad","name":"Mad"}}}
```
`ipc/players.json` -> `[{"gameId":"Mad","name":"Mad"}]`

**And it queued while the connector was down** — `core.log` was looping
`Disconnected. Reconnecting in 60s` the whole time. That confirms the design: the Lua
hook layer is fully independent of the websocket. Roster/join detection works.

### ⚠️ Q6 confirmed on Longvinter too: gameId is the NAME, no steamId
`{"gameId":"Mad","name":"Mad"}` — same weakness you flagged for Palworld. A rename
would break identity. `/Script/Longvinter.SteamIdComponent` exists; I'll dump properties
on the live PlayerState next to find the real steam id.

### ❌ Q2: chat-message did NOT fire — and I think the hook is client-side

With `ChatComponent_C:NewGlobalChatMessage` registered, I sent chat from a joined player.
**No event, no Lua error, nothing in UE4SS.log.** The hook registers but never executes.

**Hypothesis: it can't fire on a dedicated server.** The flow is
```
client types -> server GameMode NewGlobatChatMessage (broadcast) -> replicated -> EACH CLIENT's ChatComponent NewGlobalChatMessage
```
The server process runs the **GameMode** function; the ChatComponent receive is a
**client-side** handler. UE4SS is in the server, so it sees the class (hence the hook
registers) but that code path never runs there.

If that's right, your original instinct was correct and my "hook the receive, not the
broadcast" advice was **wrong for a dedicated server**. The GameMode broadcast is the only
server-side chat function.

I've swapped my local profile to
`/Game/Blueprints/Server/GM_Longvinter.GM_Longvinter_C:NewGlobatChatMessage` and it
registers; re-joining now to confirm it fires. **Do not push a profile change yet** — I'll
confirm empirically first.

If it does fire, the echo problem you raised is real but easy: `sendMessage` prefixes
`[Takaro] `, so the hook can skip messages starting with that. Suggest we do exactly that
rather than hooking a function that can't run.

### 🔧 Rig finding that will bite every game: two different Enter paths
Longvinter's chat box needed **different input methods for typing vs submitting**:
- typing works via `KEYEVENTF_UNICODE`
- **scancode Enter OPENS chat but does NOT submit it**
- a **virtual-key** Enter was required to submit

I added `{"key":"enter","backend":"vk"}` to the agent. Worth knowing when you write test
steps for other games: "send Enter" is not one operation.

### Also fixed this round
DPI. The rig laptop is a 1920x1080 panel at 125% scaling, so Windows reported 1536x864 and
every click landed ~25% off (I was changing graphics settings instead of clicking menus).
Agent is now DPI-aware; screenshot coords == click coords.

### Still open
- Q2 chat (testing the GameMode hook now)
- Q3 death — not yet attempted
- Q4 actions via injected ipc/req — not yet attempted
- Q5 identify — still parked on the dev connector

---


## 2026-07-27 00:07 UTC tester — CORRECTION + the real teleport function

### Correction: I was wrong, /Script/Longvinter.* classes DO exist

You put my "Blueprint-only, there are NO /Script/Longvinter.* classes" line into the
profile header. **That claim was too strong — please soften it.** Seven native classes
exist:
```
/Script/Longvinter.SteamIdComponent          /Script/Longvinter.LongvinterFunctionsCPP
/Script/Longvinter.ServerHostingComponent    /Script/Longvinter.LoggingComponent
/Script/Longvinter.ClosingComponent          /Script/Longvinter.GhostNPC
/Script/Longvinter.GridActor
```
What's true is narrower: the **specific** classes the old profile referenced
(`LongvinterGameState`, `LongvinterPlayerController`) don't exist, and chat/death/player
logic is all Blueprint. But the `/Script/Longvinter` namespace is real, so a `/Script`
auto-detect pass isn't useless here — it just won't find the gameplay hooks.

### Teleport: use the game's own admin function, not K2_SetActorLocation

There's a purpose-built one you'll prefer:
```
/Game/ThirdPersonCPP/Blueprints/ThirdPersonCharacter.ThirdPersonCharacter_C:ServerAdminTeleport   <- USE THIS
```
Server-authoritative and admin-intended by name. Supporting cast:
```
ThirdPersonCharacter_C:TeleportToLocation        TeleportServer / StartTeleportServer
ThirdPersonCharacter_C:TeleportToFreeLocation    TeleportPlayerToHere / TeleportTargetToOther
ThirdPersonCharacter_C:ContinueTeleport / EndTeleport   (the multi-step flow)
/Script/Longvinter.LongvinterFunctionsCPP:ClearSpaceForTeleport   (native helper)
```
`K2_SetActorLocation` / `K2_GetActorLocation` **do** exist on `/Script/Engine.Actor`, so
your current implementation should function — but raw SetActorLocation skips whatever
`ServerAdminTeleport` does (collision, ClearSpaceForTeleport, replication), so a player
could land inside geometry. I'd try `ServerAdminTeleport` first and keep K2_ as fallback.

`K2_GetActorLocation` for getPlayerLocation is fine as-is.

### Possible steamId source (handoff Q6)
`/Script/Longvinter.SteamIdComponent` exists, though its exported functions look
Steam-inventory/news related (`ConvertSteamItemInstanceIDToString`, `GetNewsImages`) —
the actual id is probably a **property**, not a UFunction, so my UFunction dump wouldn't
show it. If you want, I can dump component *properties* on a live PlayerState/Character
once a player is connected and report what holds the steam id.

### giveItem signature
Can't confirm `AdminGiveItemsServer(FName,int)` from a UFunction dump alone — param types
need reflection on the UFunction object. I'll confirm empirically via injected `ipc/req`
once a player is in, and paste whatever the Lua error says if the signature is wrong.

### Still to come: Q1-Q3
Booting the server + joining with a real client next. That's the headline result and
neither of us has it yet.

---


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
