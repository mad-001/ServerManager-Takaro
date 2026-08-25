-- ServerManager-Takaro — universal UE4SS Lua core.
-- IDENTICAL for every Unreal Engine game. The only per-game file is profile.lua.
--
-- What this does:
--   • chat + death            -> hooked via the profile's UFunction names (auto-detected
--                                when the profile leaves them blank).
--   • join / leave            -> universal roster diffing (no per-game hook needed).
--   • player roster           -> written to ipc/players.json for the DLL's getPlayers.
--   • actions from Takaro      -> polled from ipc/req, dispatched to profile.actions,
--                                answered into ipc/res.
-- All of that is exchanged with the in-DLL core (winmm.dll) over plain files, because
-- UE4SS Lua has no sockets — only io/os. See Longvinter's TakaroAgent for the pattern.

local json = require("json")

local TC = {
    MOD_DIR   = "ue4ss/Mods/TakaroConnector",
    POLL_MS   = 250,     -- how often we poll ipc/req.json for the next action
    ROSTER_MS = 3000,    -- how often we rebuild the roster + diff join/leave
    seq       = 0,
    known     = {},      -- gameId -> playerInfo  (for join/leave + death-count diffs)
    lastReqId = nil,     -- id of the last request we accepted (dedupe until it's deleted)
}
TC.IPC   = TC.MOD_DIR .. "/ipc"
TC.EVT   = TC.IPC .. "/evt"
-- Single-file request mailbox. The old multi-file protocol (ipc/req/<id>.json listed with
-- `dir /B` via io.popen) is DEAD on real UE4SS builds: io.popen is a no-op there, so the
-- directory always listed empty and NO inbound action ever ran. We use one known-path file
-- read with io.open (the only fs path proven to work): the DLL writes ipc/req.json (one
-- request; it never has more than one in flight), we answer ipc/res.json.
TC.REQ_FILE = TC.IPC .. "/req.json"
TC.RES_FILE = TC.IPC .. "/res.json"
TC.PLAYERS = TC.IPC .. "/players.json"

local function log(m) print("[Takaro] " .. tostring(m)) end

-- ── Filesystem helpers (UE4SS Lua = io/os only) ───────────────────────────────
local function winPath(p) return (p:gsub("/", "\\")) end
local function ensureDir(path)
    os.execute('cmd /c "mkdir \\"' .. winPath(path) .. '\\" 2>nul"')
end
local function listDir(path)
    local out = {}
    local h = io.popen('cmd /c "dir /B \\"' .. winPath(path) .. '\\" 2>nul"')
    if h then
        for line in h:lines() do if line ~= "" then out[#out+1] = line end end
        h:close()
    end
    return out
end
local function readFile(path)
    local f = io.open(path, "rb"); if not f then return nil end
    local d = f:read("*a"); f:close(); return d
end
local function writeAtomic(path, data)
    local tmp = path .. ".tmp"
    local f = io.open(tmp, "wb"); if not f then return false end
    f:write(data); f:close()
    os.remove(path)              -- os.rename won't overwrite on Windows
    return os.rename(tmp, path)
end
local function deleteFile(path) os.remove(path) end

-- ── Event emit: one atomically-published file per event in ipc/evt ────────────
function TC.emit(evType, data)
    TC.seq = TC.seq + 1
    local name = string.format("%s/%020d.json", TC.EVT, TC.seq)
    writeAtomic(name, json.encode({ type = evType, data = data or {} }))
end

-- ── Profile ───────────────────────────────────────────────────────────────────
-- profile.lua returns a table declaring the game-specific bits. Everything is
-- optional; whatever is missing is auto-detected or falls back to a no-op.
local profile = {}
do
    local ok, p = pcall(require, "profile")
    if ok and type(p) == "table" then profile = p
    else log("No profile.lua loaded (" .. tostring(p) .. ") — relying on auto-detect") end
end
TC.profile = profile
-- Per-game namespace for any synthesized platform id (Takaro platformId = "platform:id").
-- Prefer an explicit profile.platform slug, else the game name; never a generic "unreal".
local function slugify(s)
    s = tostring(s or ""):lower():gsub("[^%w]+", "-"):gsub("^%-+", ""):gsub("%-+$", "")
    return (s ~= "" and s) or nil
end
TC.gameSlug = slugify(profile.platform)
if not TC.gameSlug and profile.name and not profile.name:lower():find("auto%-?detect") then
    TC.gameSlug = slugify(profile.name)
end
TC.gameSlug = TC.gameSlug or "game"
local autodetect = require("autodetect")

-- ── Player roster + join/leave/death diffing ──────────────────────────────────
-- profile.players() must return an array of { gameId, name, steamId?, platformId?, deaths? }.
-- Falls back to the universal FindAllOf("PlayerState") roster when the profile has none,
-- so join/leave/getPlayers work on almost any UE game with no per-game code.
local function getRoster()
    local fn = (type(profile.players) == "function") and profile.players or autodetect.players
    local ok, list = pcall(fn)
    if not ok or type(list) ~= "table" then return {} end
    return list
end

-- Takaro requires at least one platform identifier (steamId / epicOnlineServicesId /
-- xboxLiveId / platformId) to CREATE a player. Without one, join/leave/chat/death events
-- pass the connector's DTO validation but are DROPPED in the player-sync worker (so they
-- silently never appear — no error comes back to the mod). Guarantee one on every player:
-- prefer a real steam/epic id, else synthesize a stable "unreal:<gameId>" (valid per
-- Takaro's platformId regex) so events always resolve to a player.
local function sanitizeId(s) return (tostring(s or ""):gsub("[^%w_%-]", "_")) end
local function platformIdFor(p)
    if p.platformId and p.platformId ~= "" then return p.platformId end
    if p.steamId and p.steamId ~= "" then return "steam:" .. p.steamId end
    if p.epicOnlineServicesId and p.epicOnlineServicesId ~= "" then return "epic:" .. p.epicOnlineServicesId end
    local gid = sanitizeId(p.gameId); if gid == "" then gid = "unknown" end
    return (TC.gameSlug or "game") .. ":" .. gid
end
-- Canonical IGamePlayer for events + roster. ONLY DTO-whitelisted fields (no deaths etc.,
-- or the connector's forbidNonWhitelisted validation would reject the event).
function TC.playerObj(p)
    local o = { gameId = tostring(p.gameId or ""), name = p.name or "", platformId = platformIdFor(p) }
    if p.steamId and p.steamId ~= "" then o.steamId = p.steamId end
    if p.epicOnlineServicesId and p.epicOnlineServicesId ~= "" then o.epicOnlineServicesId = p.epicOnlineServicesId end
    return o
end

local function publishRosterAndDiff()
    -- Roster diff for join/leave/death; also publishes players.json for the DLL's getPlayers.
    local raw = getRoster()
    local list, deathsById = {}, {}
    for _, p in ipairs(raw) do
        if p.gameId then
            local o = TC.playerObj(p)
            list[#list + 1] = o
            deathsById[o.gameId] = p.deaths
        end
    end
    -- publish for the DLL's getPlayers (force [] for an empty roster — an empty Lua
    -- table would encode as {} and the DLL only accepts a JSON array)
    writeAtomic(TC.PLAYERS, (#list == 0) and "[]" or json.encode(list))
    -- diff for join / leave / death
    local current = {}
    for _, p in ipairs(list) do
        local id = p.gameId
        current[id] = true
        local prev = TC.known[id]
        local d = deathsById[id]
        if not prev then
            TC.emit("player-connected", { player = p })
        elseif d and prev.deaths and tonumber(d) > tonumber(prev.deaths) then
            TC.emit("player-death", { player = p })
        end
        TC.known[id] = { name = p.name, platformId = p.platformId, steamId = p.steamId,
                         epicOnlineServicesId = p.epicOnlineServicesId, deaths = d or (prev and prev.deaths) or 0 }
    end
    for id, prev in pairs(TC.known) do
        if not current[id] then
            TC.emit("player-disconnected", { player = { gameId = id, name = prev.name,
                platformId = prev.platformId, steamId = prev.steamId, epicOnlineServicesId = prev.epicOnlineServicesId } })
            TC.known[id] = nil
        end
    end
end

-- ── Chat + death hooks (profile-driven, else auto-detected) ───────────────────
-- Resolve spec.hook: if the profile gave an explicit hook use it; otherwise probe
-- spec.candidates (a list of "/Script/..." paths) live and pick the first that exists.
local function resolveHook(spec)
    if spec.hook then return spec.hook end
    if type(spec.candidates) == "table" then
        for _, path in ipairs(spec.candidates) do
            local ok, o = pcall(function() return StaticFindObject(path) end)
            if ok and o and o:IsValid() then
                log("hook resolved -> " .. path)
                return path
            end
        end
        log("none of " .. #spec.candidates .. " candidate hooks resolved")
    end
    return nil
end

-- ── Echo suppression ──────────────────────────────────────────────────────────
-- Takaro's sendMessage makes us broadcast in-game, which trips our OWN chat hook and
-- would bounce straight back as an inbound chat-message (a feedback loop the instant a
-- module echoes chat). Record what we send and drop any incoming line carrying that text
-- for a short window. Profile-agnostic: works whatever prefix (if any) a profile adds.
TC.recentSends = {}   -- { {text=, at=}, ... }, most-recent last
local function nowSecs() local ok, t = pcall(os.time); return ok and t or 0 end
local function recordSend(text)
    text = tostring(text or "")
    if text == "" then return end
    local now, keep = nowSecs(), {}
    for _, e in ipairs(TC.recentSends) do if now - e.at <= 15 then keep[#keep+1] = e end end
    keep[#keep+1] = { text = text, at = now }
    while #keep > 8 do table.remove(keep, 1) end
    TC.recentSends = keep
end
local function isEcho(msg)
    msg = tostring(msg or "")
    if msg == "" then return false end
    for _, e in ipairs(TC.recentSends) do
        -- our broadcast may be prefixed (e.g. "[Takaro] "), so match by containment too
        if e.text ~= "" and (msg == e.text or msg:find(e.text, 1, true)) then return true end
    end
    return false
end

local function installChat()
    local spec = profile.chat
    if spec then
        spec.hook = resolveHook(spec)
        -- profile chat given but nothing resolved -> fall back to universal runtime discovery
        if not spec.hook then spec = autodetect.chat() end
    else
        spec = autodetect.chat()   -- no profile chat: universal runtime discovery
    end
    if not spec or not spec.hook then log("Chat: no hook resolved (roster/join/leave still work)"); return end
    local ok, err = pcall(function()
        RegisterHook(spec.hook, function(self, a, b, c)
            local sok, serr = pcall(function()
                local name, msg, channel = spec.extract(self, a, b, c)
                if not msg or msg == "" then return end
                if msg:sub(1,1) == "/" then return end          -- skip slash-commands
                if isEcho(msg) then return end                  -- skip our own broadcasts (no feedback loop)
                -- A single-FString server broadcast has no sender; emit an empty player
                -- rather than duplicating the message text into name/gameId.
                local gid = spec.gameId and spec.gameId(self,a,b,c) or name
                -- player is optional on chat-message; only include one when we have a
                -- sender, and give it a valid platform id so it resolves.
                local pl = nil
                if (name and name ~= "") or (gid and gid ~= "") then
                    pl = TC.playerObj({ gameId = gid or name, name = name })
                end
                TC.emit("chat-message", {
                    player  = pl,
                    msg     = msg,
                    channel = channel or "global",
                })
            end)
            if not sok then log("chat hook error: " .. tostring(serr)) end
        end)
    end)
    if ok then log("Chat hook: " .. spec.hook) else log("Chat hook FAILED (" .. spec.hook .. "): " .. tostring(err)) end
end

local function installDeath()
    local spec = profile.death
    if not spec then spec = autodetect.death() end
    if not spec then log("Death: no spec (using roster death-count diff if available)"); return end
    spec.hook = resolveHook(spec)   -- supports spec.hook or spec.candidates
    if not spec.hook then log("Death: no hook resolved (roster diff still applies)"); return end
    local ok, err = pcall(function()
        RegisterHook(spec.hook, function(self, a, b, c)
            local sok, serr = pcall(function()
                local name, gameId, steamId = spec.extract(self, a, b, c)
                if not name and not gameId then return end
                TC.emit("player-death", { player = TC.playerObj({ gameId = gameId or name, name = name, steamId = steamId }) })
            end)
            if not sok then log("death hook error: " .. tostring(serr)) end
        end)
    end)
    if ok then log("Death hook: " .. spec.hook) else log("Death hook FAILED (" .. spec.hook .. "): " .. tostring(err)) end
end

-- ── Action dispatch (Takaro -> game) ──────────────────────────────────────────
-- ipc/req/<id>.json = {action, args}. We answer ipc/res/<id>.json = {success,result,error}.

-- UE console command via the live world (works on ~any UE server; runs on the game thread
-- because processReq wraps dispatch in ExecuteInGameThread).
local function consoleCommand(cmd)
    local ok, err = pcall(function()
        local ksl = StaticFindObject("/Script/Engine.Default__KismetSystemLibrary")
        local world = FindFirstOf("World")
        if not (ksl and world and world:IsValid()) then error("no World/KismetSystemLibrary") end
        ksl:ExecuteConsoleCommand(world, cmd, nil)
    end)
    return ok, err
end

-- ── UFunction reflection (READ-ONLY — never calls the function) ────────────────
-- A wrong-signature UFunction call hard-hangs the game thread and pcall can't catch it
-- (BUG 4). Reflecting params first lets a handler refuse to blind-call an unknown shape.
function TC.reflectParams(func)
    if not func then return nil end
    if not pcall(function() return func:IsValid() end) then return nil end
    local params = {}
    local ok = pcall(function()
        func:ForEachProperty(function(prop)
            local name = "?"; pcall(function() name = prop:GetFName():ToString() end)
            local ptype = "?"; pcall(function() ptype = prop:GetClass():GetFName():ToString() end)
            local ret = false; pcall(function() ret = prop:HasAnyPropertyFlags(0x400) end)  -- CPF_ReturnParm
            params[#params+1] = { name = name, type = ptype, ret = ret }
        end)
    end)
    if not ok then return nil end
    return params
end
function TC.findFuncOnClass(obj, fname)
    local target = nil
    pcall(function()
        obj:GetClass():ForEachFunction(function(f)
            local n; pcall(function() n = f:GetFName():ToString() end)
            if n == fname then target = f end
        end)
    end)
    return target
end

-- Universal fallbacks — used when the profile doesn't define its own handler AND the DLL
-- didn't already handle the action over RCON. When RCON is configured (RCON_PORT in
-- TakaroConfig.txt) the core runs executeConsoleCommand/shutdown against the game's own
-- RCON before it ever reaches here; these are the no-RCON path (mod is IN the process).
local builtins = {}
-- reflect: dump a UFunction's params WITHOUT calling it. Pass args.path (full object path)
-- OR args.class + args.func (finds a live instance of the class, then the function).
builtins.reflect = function(args)
    local func = nil
    if args.path and args.path ~= "" then pcall(function() func = StaticFindObject(tostring(args.path)) end) end
    if (not func) and args.class and args.func then
        local inst; pcall(function() inst = FindFirstOf(tostring(args.class)) end)
        if inst and inst:IsValid() then func = TC.findFuncOnClass(inst, tostring(args.func)) end
    end
    if not func then return false, "reflect: not found (give args.path OR args.class+args.func)" end
    local p = TC.reflectParams(func)
    if not p then return false, "reflect: could not enumerate params (ForEachProperty unavailable on this build)" end
    return true, { success = true, params = p }
end
builtins.shutdown = function()
    local ok, err = consoleCommand("quit")          -- graceful engine shutdown (saves)
    if ok then return true, { success = true, rawResult = "shutdown (console quit)" } end
    return false, { success = false, rawResult = "shutdown failed: " .. tostring(err) }
end
-- Takaro's console action is "executeConsoleCommand" with {command}; expects {success,rawResult}.
builtins.executeConsoleCommand = function(args)
    local cmd = tostring(args.command or args.rawCommand or args.message or "")
    if cmd == "" then return false, { success = false, rawResult = "empty command" } end
    local ok, err = consoleCommand(cmd)
    if ok then return true, { success = true, rawResult = "ran: " .. cmd } end
    return false, { success = false, rawResult = tostring(err) }
end
builtins.executeCommand = builtins.executeConsoleCommand   -- alias for older callers

local function dispatch(action, args)
    local handlers = profile.actions or {}
    local h = handlers[action] or builtins[action]   -- profile overrides; else universal builtin
    if not h then
        return { success = false, error = "Action not implemented in profile: " .. tostring(action) }
    end
    -- record outbound chat BEFORE broadcasting so the echo-guard is armed when our own
    -- message trips the chat hook a moment later (both run on the game thread).
    if action == "sendMessage" then recordSend((args or {}).message or (args or {}).msg) end
    local ok, a, b = pcall(h, args or {})
    if not ok then return { success = false, error = tostring(a) } end
    -- handler may return (result) or (success, message/result)
    if type(a) == "table" then return { success = true, result = a } end
    if type(a) == "boolean" then return { success = a, result = (a and (b or {}) or nil), error = (not a) and tostring(b) or nil } end
    return { success = true, result = (a ~= nil and a or {}) }
end

-- Read the single request mailbox (io.open only — no directory listing). The DLL writes
-- ipc/req.json = {id, action, args} and blocks until ipc/res.json (matching id) appears,
-- so there's never more than one request pending. We dedupe on id so we don't reprocess
-- the same file across poll ticks before the game thread deletes it.
local function processReqFile()
    local raw = readFile(TC.REQ_FILE)
    if not raw then return end
    local msg = json.decode(raw)
    if not msg or not msg.action then deleteFile(TC.REQ_FILE); return end
    local id = tostring(msg.id or "0")
    if id == TC.lastReqId then return end        -- already accepted; ignore until overwritten
    TC.lastReqId = id
    -- Consume the request from the mailbox NOW, on the poll thread, BEFORE handing the work
    -- to the game thread. If a handler ever stalls or dies on the game thread (e.g. a bad
    -- UFunction call — see BUG 4: a wrong-signature call hard-hangs and pcall can't catch it),
    -- the mailbox is already free: the next request still gets read, and the DLL sees an
    -- honest per-action timeout for the stalled one instead of the whole inbound path silently
    -- wedging on a stale file. (Game mutations still run on the game thread.)
    deleteFile(TC.REQ_FILE)
    ExecuteInGameThread(function()
        local ok, out = pcall(dispatch, msg.action, msg.args)
        local res = ok and out or { success = false, error = tostring(out) }
        if type(res) ~= "table" then res = { success = true, result = res } end
        res.id = id                              -- DLL accepts only the matching id
        pcall(function() writeAtomic(TC.RES_FILE, json.encode(res)) end)
    end)
end

-- ── Boot ──────────────────────────────────────────────────────────────────────
print("=======================================")
print(" ServerManager-Takaro connector loaded")
print("  profile: " .. (profile.name or "(none / auto-detect)"))
print("=======================================")

ensureDir(TC.EVT)
-- clear any stale mailbox from a previous run (known paths; no directory listing)
deleteFile(TC.REQ_FILE); deleteFile(TC.RES_FILE)

-- install hooks once the game world is up (native /Script UFunctions are ready at
-- startup; some RPCs load later, so defer a little and let auto-detect retry).
ExecuteWithDelay(4000, function()
    installChat()
    installDeath()
    if type(profile.init) == "function" then pcall(profile.init, TC) end
end)

-- action poll loop (single mailbox; io.open only)
LoopAsync(TC.POLL_MS, function()
    local ok, err = pcall(processReqFile)
    if not ok then log("req poll error: " .. tostring(err)) end
    return false
end)

-- roster loop (join/leave/death diff + players.json)
ExecuteWithDelay(5000, function()
    LoopAsync(TC.ROSTER_MS, function()
        pcall(publishRosterAndDiff)
        return false
    end)
end)

log("connector ready")
return TC
