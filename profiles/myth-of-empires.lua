-- Myth of Empires profile for ServerManager-Takaro
-- Engine: Unreal Engine 4. Confidence: low (module name guessed).
-- NOTE: the manager DB flagged the GAME app (1371580) as owned-gated, but Myth of Empires
-- ships a FREE anonymous dedicated server (Steam app 1794810) — so it IS in scope.
-- Join/leave/roster/death work via the universal core; chat is a live-probe guess.
local function genericExtract(self, param)
    local ok, s = pcall(function() return param:get() end)
    local o = ok and s or param
    local function str(v) local a, r = pcall(function() return v:ToString() end); return a and r or nil end
    local nm, msg
    for _, k in ipairs({ "Sender", "PlayerName", "Name" }) do local a,v=pcall(function() return o[k] end); if a and v then nm=str(v) or nm end if nm then break end end
    for _, k in ipairs({ "Message", "Text", "Content" }) do local a,v=pcall(function() return o[k] end); if a and v then msg=str(v) or msg end if msg then break end end
    return nm, msg, "global"
end
return {
    name = "Myth of Empires",
    chat = {
        candidates = {
            "/Script/MOE.MOEGameState:BroadcastChatMessage",
            "/Script/Angela.AngelaGameState:BroadcastChatMessage",
            "/Script/MythOfEmpires.MythOfEmpiresGameState:BroadcastChatMessage",
            "/Script/MOE.MOEPlayerController:ClientReceiveChatMessage",
        },
        extract = genericExtract,
    },
}
