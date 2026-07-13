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

-- Returns a chat spec {hook, extract} or nil.
function M.chat()
    for _, path in ipairs(CHAT_CANDIDATES) do
        if resolves(path) then
            print("[Takaro] auto-detect: chat candidate resolves -> " .. path)
            return { hook = path, extract = genericExtract }
        end
    end
    print("[Takaro] auto-detect: no known chat UFunction resolved — write profile.chat for this game")
    return nil
end

-- Death is normally handled by the roster death-count diff in main.lua, so return nil.
function M.death()
    return nil
end

return M
