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
    POLL_MS   = 250,     -- how often we poll ipc/req for actions
    ROSTER_MS = 3000,    -- how often we rebuild the roster + diff join/leave
    seq       = 0,
    known     = {},      -- gameId -> playerInfo  (for join/leave + death-count diffs)
}
TC.IPC   = TC.MOD_DIR .. "/ipc"
TC.EVT   = TC.IPC .. "/evt"
TC.REQ   = TC.IPC .. "/req"
TC.RES   = TC.IPC .. "/res"
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
local autodetect = require("autodetect")

-- ── Player roster + join/leave/death diffing ──────────────────────────────────
-- profile.players() must return an array of { gameId, name, steamId?, platformId?, deaths? }.
local function getRoster()
    if type(profile.players) ~= "function" then return {} end
    local ok, list = pcall(profile.players)
    if not ok or type(list) ~= "table" then return {} end
    return list
end

local function publishRosterAndDiff()
    local list = getRoster()
    -- publish for the DLL's getPlayers
    writeAtomic(TC.PLAYERS, json.encode(list))
    -- diff for join / leave / death
    local current = {}
    for _, p in ipairs(list) do
        if p.gameId then
            local id = tostring(p.gameId)
            current[id] = true
            local prev = TC.known[id]
            if not prev then
                TC.emit("player-connected", { player = { gameId = id, name = p.name, steamId = p.steamId } })
            elseif p.deaths and prev.deaths and tonumber(p.deaths) > tonumber(prev.deaths) then
                TC.emit("player-death", { player = { gameId = id, name = p.name, steamId = p.steamId } })
            end
            TC.known[id] = { name = p.name, steamId = p.steamId, deaths = p.deaths or (prev and prev.deaths) or 0 }
        end
    end
    for id, prev in pairs(TC.known) do
        if not current[id] then
            TC.emit("player-disconnected", { player = { gameId = id, name = prev.name, steamId = prev.steamId } })
            TC.known[id] = nil
        end
    end
end

-- ── Chat + death hooks (profile-driven, else auto-detected) ───────────────────
local function installChat()
    local spec = profile.chat
    if not spec then spec = autodetect.chat() end
    if not spec or not spec.hook then log("Chat: no hook (profile.chat missing + auto-detect found none)"); return end
    local ok, err = pcall(function()
        RegisterHook(spec.hook, function(self, a, b, c)
            local sok, serr = pcall(function()
                local name, msg, channel = spec.extract(self, a, b, c)
                if not msg or msg == "" then return end
                if msg:sub(1,1) == "/" then return end          -- skip slash-commands
                TC.emit("chat-message", {
                    player  = { name = name or "", gameId = spec.gameId and spec.gameId(self,a,b,c) or (name or "") },
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
    if not spec or not spec.hook then log("Death: no hook (using roster death-count diff if available)"); return end
    local ok, err = pcall(function()
        RegisterHook(spec.hook, function(self, a, b, c)
            local sok, serr = pcall(function()
                local name, gameId, steamId = spec.extract(self, a, b, c)
                if not name and not gameId then return end
                TC.emit("player-death", { player = { name = name or "", gameId = tostring(gameId or name or ""), steamId = steamId } })
            end)
            if not sok then log("death hook error: " .. tostring(serr)) end
        end)
    end)
    if ok then log("Death hook: " .. spec.hook) else log("Death hook FAILED (" .. spec.hook .. "): " .. tostring(err)) end
end

-- ── Action dispatch (Takaro -> game) ──────────────────────────────────────────
-- ipc/req/<id>.json = {action, args}. We answer ipc/res/<id>.json = {success,result,error}.
local function dispatch(action, args)
    local handlers = profile.actions or {}
    local h = handlers[action]
    if not h then
        return { success = false, error = "Action not implemented in profile: " .. tostring(action) }
    end
    local ok, a, b = pcall(h, args or {})
    if not ok then return { success = false, error = tostring(a) } end
    -- handler may return (result) or (success, message/result)
    if type(a) == "table" then return { success = true, result = a } end
    if type(a) == "boolean" then return { success = a, result = (a and (b or {}) or nil), error = (not a) and tostring(b) or nil } end
    return { success = true, result = (a ~= nil and a or {}) }
end

local function processReq(filename)
    local reqPath = TC.REQ .. "/" .. filename
    local raw = readFile(reqPath)
    if not raw then return end
    local msg = json.decode(raw)
    if not msg or not msg.action then deleteFile(reqPath); return end
    local id = filename:gsub("%.json$", "")
    -- game mutations must run on the game thread
    ExecuteInGameThread(function()
        local res
        local ok, out = pcall(dispatch, msg.action, msg.args)
        res = ok and out or { success = false, error = tostring(out) }
        writeAtomic(TC.RES .. "/" .. id .. ".json", json.encode(res))
        deleteFile(reqPath)
    end)
end

-- ── Boot ──────────────────────────────────────────────────────────────────────
print("=======================================")
print(" ServerManager-Takaro connector loaded")
print("  profile: " .. (profile.name or "(none / auto-detect)"))
print("=======================================")

ensureDir(TC.EVT); ensureDir(TC.REQ); ensureDir(TC.RES)
-- clear any stale req/res from a previous run (evt is drained by the DLL)
for _, f in ipairs(listDir(TC.REQ)) do deleteFile(TC.REQ .. "/" .. f) end
for _, f in ipairs(listDir(TC.RES)) do deleteFile(TC.RES .. "/" .. f) end

-- install hooks once the game world is up (native /Script UFunctions are ready at
-- startup; some RPCs load later, so defer a little and let auto-detect retry).
ExecuteWithDelay(4000, function()
    installChat()
    installDeath()
    if type(profile.init) == "function" then pcall(profile.init, TC) end
end)

-- action poll loop
LoopAsync(TC.POLL_MS, function()
    local ok, err = pcall(function()
        for _, name in ipairs(listDir(TC.REQ)) do
            if name:match("%.json$") then processReq(name) end
        end
    end)
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
