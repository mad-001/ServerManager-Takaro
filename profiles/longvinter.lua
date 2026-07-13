-- Longvinter profile for ServerManager-Takaro
-- Engine: Unreal Engine 4. Confidence: medium.
-- Actions use the real dedicated-server GameMode AGM_Longvinter_C (verified from the
-- existing Longvinter TakaroAgent): NewGlobatChatMessage(FString) [sic], KickBySteamID,
-- BanBySteamID, RestartServer, SaveGame. Player join/leave/roster come from the universal
-- core. CHAT capture (incoming) is a live-probe guess — confirm with a UFunction dump.

local function gm()
    local g = FindFirstOf("GM_Longvinter_C")
    if g and g:IsValid() then return g end
    return nil
end

return {
    name = "Longvinter",
    chat = {
        candidates = {
            "/Game/Blueprints/GameModes/GM_Longvinter.GM_Longvinter_C:NewGlobatChatMessage",
            "/Script/Longvinter.LongvinterGameState:BroadcastChatMessage",
            "/Script/Longvinter.LongvinterPlayerController:ClientReceiveChatMessage",
        },
        extract = function(self, param)
            local ok, s = pcall(function() return param:get() end)
            local o = ok and s or param
            local function str(v) local a, r = pcall(function() return v:ToString() end); return a and r or nil end
            return str(o and o.Sender) or str(o and o.PlayerName), str(o and o.Message) or str(o and o.Text), "global"
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
