-- Longvinter profile for ServerManager-Takaro
-- Engine: Unreal Engine 4 (Blueprint-only — there are NO /Script/Longvinter.* classes).
-- Hook paths VERIFIED via a live 250k-UObject dump on a running dedicated server (tester).
--   chat receive : /Game/ThirdPersonCPP/Blueprints/ChatComponent.ChatComponent_C:NewGlobalChatMessage
--   team chat    : /Game/ThirdPersonCPP/Blueprints/ChatComponent.ChatComponent_C:NewTeamChatMessage
--   death        : /Game/ThirdPersonCPP/Blueprints/ThirdPersonCharacter.ThirdPersonCharacter_C:DeathServer
--   broadcast    : /Game/Blueprints/Server/GM_Longvinter.GM_Longvinter_C:NewGlobatChatMessage  [sic]
-- NOTE: hook the RECEIVE fn for chat capture; the misspelled GameMode NewGlobatChatMessage is the
-- server BROADCAST (used by sendMessage) — hooking it would echo Takaro's own messages back.

local function str(v) local o, r = pcall(function() return v:ToString() end); return o and r or nil end

-- best-effort field extraction (confirm field names against a live chat once it fires)
local function extractChat(self, param, channel)
    local ok, s = pcall(function() return param:get() end)
    local o = ok and s or param
    local name, msg
    for _, k in ipairs({ "PlayerName", "Sender", "Name", "SenderName", "Player" }) do
        local a, v = pcall(function() return o[k] end); if a and v then name = str(v) or name end
        if name then break end
    end
    for _, k in ipairs({ "Message", "Text", "Msg", "ChatMessage", "Content" }) do
        local a, v = pcall(function() return o[k] end); if a and v then msg = str(v) or msg end
        if msg then break end
    end
    return name, msg, channel or "global"
end

local function gm()
    local g = FindFirstOf("GM_Longvinter_C"); if g and g:IsValid() then return g end; return nil
end

return {
    name = "Longvinter",

    chat = {
        hook = "/Game/ThirdPersonCPP/Blueprints/ChatComponent.ChatComponent_C:NewGlobalChatMessage",
        extract = function(self, param) return extractChat(self, param, "global") end,
    },

    death = {
        hook = "/Game/ThirdPersonCPP/Blueprints/ThirdPersonCharacter.ThirdPersonCharacter_C:DeathServer",
        extract = function(character)
            local nm
            pcall(function() nm = character.PlayerState.PlayerNamePrivate:ToString() end)
            if not nm then pcall(function() nm = character:GetPlayerName():ToString() end) end
            return nm, nm
        end,
    },

    actions = {
        sendMessage = function(args)
            local g = gm(); if not g then return false, "GameMode not found" end
            g:NewGlobatChatMessage("[Takaro] " .. tostring(args.message or args.msg or ""))
            return true, "broadcast"
        end,
        kickPlayer = function(args)
            local g = gm(); if not g then return false, "GameMode not found" end
            local id = tostring(args.steamId or args.gameId or (args.player and (args.player.steamId or args.player.gameId)) or "")
            g:KickBySteamID(id); return true, "kicked " .. id
        end,
        banPlayer = function(args)
            local g = gm(); if not g then return false, "GameMode not found" end
            local id = tostring(args.steamId or args.gameId or (args.player and (args.player.steamId or args.player.gameId)) or "")
            g:BanBySteamID(id); return true, "banned " .. id
        end,
    },
}
