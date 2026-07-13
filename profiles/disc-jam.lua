-- Disc Jam profile for ServerManager-Takaro
-- Engine: ue4. Confidence: low (module name guessed from title).
-- WHAT WORKS OUT OF THE BOX (no live confirmation needed): player join/leave, roster /
--   getPlayers, and death (via the universal core's FindAllOf("PlayerState") roster).
-- CHAT is best-effort: the core probes the candidate hooks below on your live server and
--   uses whichever exists. If none resolve, dump the game's UFunctions with UE4SS
--   (Objects dumper) and set an explicit chat.hook. The /Script module was guessed as 'DiscJam' from the game title and is very likely wrong for chat — expect to set chat.hook after a UFunction dump.

local function genericExtract(self, param)
    local ok, s = pcall(function() return param:get() end)
    local obj = ok and s or param
    local function str(v) local o, r = pcall(function() return v:ToString() end); return o and r or nil end
    local name, msg
    for _, k in ipairs({ "Sender", "PlayerName", "Name", "From", "SenderName" }) do
        local o, v = pcall(function() return obj[k] end); if o and v then name = str(v) or name end
        if name then break end
    end
    for _, k in ipairs({ "Message", "Text", "Msg", "Content", "ChatMessage" }) do
        local o, v = pcall(function() return obj[k] end); if o and v then msg = str(v) or msg end
        if msg then break end
    end
    return name, msg, "global"
end

return {
    name = "Disc Jam",
    -- players()/join/leave/death: handled by the universal core. Override here only if the
    -- universal PlayerState roster misses this game.
    chat = {
        candidates = {
            "/Script/DiscJam.DiscJamGameStateInGame:BroadcastChatMessage",
            "/Script/DiscJam.DiscJamGameState:BroadcastChatMessage",
            "/Script/DiscJam.DiscJamGameState:Multicast_ChatMessage",
            "/Script/DiscJam.DiscJamPlayerController:ClientReceiveChatMessage",
            "/Script/DiscJam.DiscJamPlayerController:ServerSendChatMessage",
            "/Script/DiscJam.DiscJamGameMode:ReceiveChatMessage",
            "/Script/DiscJam.DiscJamPlayerController:ServerSay",
            "/Script/DiscJam.DiscJamPlayerController:ClientMessage"
        },
        extract = genericExtract,
    },
}
