-- Best-effort auto-detection of chat/death UFunctions for games without a full profile.
--
-- Honesty note: reliable chat capture almost always needs a per-game profile, because
-- the parameter struct field names differ per game. Auto-detect exists so a brand-new
-- game still stands a chance out of the box, and so operators get told which candidate
-- functions exist. Death usually needs NO hook at all — main.lua derives it from the
-- roster's death counter when the profile's players() reports one.

local M = {}

-- Full UFunction paths seen to carry a chat broadcast across shipped UE servers.
-- Extend this list as new games are profiled.
local CHAT_CANDIDATES = {
    "/Script/Pal.PalGameStateInGame:BroadcastChatMessage",
    "/Script/Pal.PalPlayerState:EnterChat_Receive",
}

-- Generic extractor: try the common (name, message) field shapes on the param struct.
local function genericExtract(self, param)
    local ok, s = pcall(function() return param:get() end)
    local obj = ok and s or param
    local function str(v) local o,r = pcall(function() return v:ToString() end); return o and r or nil end
    local name, msg
    for _, k in ipairs({ "Sender", "PlayerName", "Name", "From", "Author" }) do
        local o, v = pcall(function() return obj[k] end)
        if o and v then name = str(v) or name; if name then break end end
    end
    for _, k in ipairs({ "Message", "Text", "Msg", "Content", "ChatMessage" }) do
        local o, v = pcall(function() return obj[k] end)
        if o and v then msg = str(v) or msg; if msg then break end end
    end
    return name, msg, "global"
end

local function resolves(path)
    local ok, o = pcall(function() return StaticFindObject(path) end)
    return ok and o and o:IsValid()
end

-- Common chat-broadcast/receive UFunction short names, tried against the game's REAL
-- classes (discovered at runtime), newest/most-specific first.
local CHAT_FUNCS = {
    "BroadcastChatMessage", "ServerBroadcastChatMessage", "Multicast_ChatMessage",
    "ReceiveChatMessage", "ClientReceiveChatMessage", "ServerSendChatMessage",
    "OnChatMessage", "AddChatMessage", "HandleChatMessage", "ServerSay",
    "ServerChat", "ClientMessage", "BroadcastMessage",
    -- broadened for the sweep: more shipped-game chat entry points
    "Server_SendChatMessage", "ServerSendMessage", "OnReceiveChatMessage",
    "ReceiveMessage", "OnChatMessageReceived", "SendChatMessage", "SendChat",
    "ClientReceiveMessage", "Multicast_OnChatMessage", "RPC_ChatMessage",
    "ServerPostChatMessage", "PostChatMessage", "NewChatMessage", "OnMessageReceived",
    "ChatMessageReceived", "Server_Chat", "Client_Chat", "AddChatMessage_Multicast",
}

-- Turn a live object's class into its /Script path, e.g.
--   "Class /Script/Pal.PalGameStateInGame" -> "/Script/Pal.PalGameStateInGame"
local function classPath(obj)
    local ok, full = pcall(function() return obj:GetClass():GetFullName() end)
    if not ok or not full then return nil end
    return full:match("%s(/[%w_%./]+)$") or full:match("(/[%w_%./]+)$")
end

-- Discover the game's real GameState / GameMode / PlayerController / PlayerState classes
-- from live instances — no per-game guessing needed.
local function discoverClasses()
    local paths, seen = {}, {}
    for _, cls in ipairs({ "GameStateBase", "GameState", "GameModeBase", "GameMode",
                           "PlayerController", "PlayerState" }) do
        local ok, inst = pcall(function() return FindFirstOf(cls) end)
        if ok and inst and inst:IsValid() then
            local p = classPath(inst)
            if p and not seen[p] then seen[p] = true; paths[#paths + 1] = p end
        end
    end
    return paths
end

-- Returns a chat spec {hook, extract} or nil. First tries the game's real classes ×
-- common chat function names, then the hard-coded candidate list.
function M.chat()
    for _, base in ipairs(discoverClasses()) do
        for _, fn in ipairs(CHAT_FUNCS) do
            local path = base .. ":" .. fn
            if resolves(path) then
                print("[Takaro] auto-detect: chat resolved -> " .. path)
                return { hook = path, extract = genericExtract }
            end
        end
    end
    for _, path in ipairs(CHAT_CANDIDATES) do
        if resolves(path) then
            print("[Takaro] auto-detect: chat candidate resolves -> " .. path)
            return { hook = path, extract = genericExtract }
        end
    end
    print("[Takaro] auto-detect: no chat UFunction resolved — dump UFunctions and set profile.chat.hook")
    return nil
end

-- Common death UFunction short names, tried against the game's real character classes.
local DEATH_FUNCS = {
    "OnDeadPlayer_Server", "OnDeadCharacter", "OnDead", "OnDeath", "OnDying",
    "HandleDeath", "Died", "Die", "Death", "ServerDie", "K2_OnDeath",
    -- broadened for the sweep
    "OnPlayerDeath", "OnPlayerDied", "OnCharacterDeath", "OnCharacterDied",
    "HandlePlayerDeath", "PlayerDied", "ServerOnDeath", "Multicast_OnDeath",
    "OnDeathEvent", "NotifyDeath", "OnKilled", "Killed", "HandleDdeath_Server",
    "OnPawnDied", "ServerDeath", "Client_OnDeath",
}

-- Discover a death hook generically: probe common death function names against the
-- game's live character/pawn/playerstate classes. Returns {hook, extract} or nil so
-- main.lua falls back to the roster death-count diff.
function M.death()
    local bases = {}
    for _, cls in ipairs({ "Character", "Pawn", "PlayerState", "PlayerController" }) do
        local ok, inst = pcall(function() return FindFirstOf(cls) end)
        if ok and inst and inst:IsValid() then
            local ok2, full = pcall(function() return inst:GetClass():GetFullName() end)
            if ok2 and full then
                local p = full:match("%s(/[%w_%./]+)$")
                if p then bases[#bases + 1] = p end
            end
        end
    end
    for _, base in ipairs(bases) do
        for _, fn in ipairs(DEATH_FUNCS) do
            local path = base .. ":" .. fn
            local ok, o = pcall(function() return StaticFindObject(path) end)
            if ok and o and o:IsValid() then
                print("[Takaro] auto-detect: death resolved -> " .. path)
                return {
                    hook = path,
                    extract = function(self)
                        local nm
                        pcall(function() nm = self.PlayerState.PlayerNamePrivate:ToString() end)
                        if not nm then pcall(function() nm = self:GetPlayerName():ToString() end) end
                        return nm, nm
                    end,
                }
            end
        end
    end
    return nil
end

-- UNIVERSAL roster — works on almost any Unreal game with no profile.
-- APlayerState::PlayerNamePrivate is engine-standard, and UE4SS FindAllOf matches
-- subclasses, so FindAllOf("PlayerState") enumerates every game's players. Used by
-- main.lua whenever the profile doesn't define its own players().
function M.players()
    local out = {}
    local ok, list = pcall(function() return FindAllOf("PlayerState") end)
    if not ok or not list then return out end
    for _, ps in ipairs(list) do
        if ps and ps:IsValid() then
            local n
            local o1 = pcall(function() n = ps.PlayerNamePrivate:ToString() end)
            if (not o1 or not n or n == "") then
                pcall(function() n = ps:GetPlayerName():ToString() end)
            end
            if n and n ~= "" then
                -- try a stable unique net id for gameId/steamId; fall back to the name
                local uid
                pcall(function() uid = ps.UniqueId:ToString() end)
                out[#out + 1] = { gameId = uid or n, name = n, steamId = (uid and uid:match("(%d%d%d%d%d+)")) }
            end
        end
    end
    return out
end

return M
