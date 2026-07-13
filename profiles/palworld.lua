-- Palworld profile for ServerManager-Takaro.
-- Ported from the working TakaroChat UE4SS mod — all engine-native, no bridge.
-- UE5 / /Script/Pal. Rename to profile.lua next to main.lua to use.

local function nameOf(playerState)
    local ok, n = pcall(function() return playerState.PlayerNamePrivate:ToString() end)
    return ok and n or nil
end

-- Find an online PalPlayerCharacter by Takaro gameId (== player name here).
local function findByGameId(gameId)
    local list = FindAllOf("PalPlayerCharacter")
    if not list then return nil end
    for _, P in ipairs(list) do
        if P and P:IsValid() and P.PlayerState and P.PlayerState:IsValid() then
            if nameOf(P.PlayerState) == gameId then return P end
        end
    end
    return nil
end

return {
    name = "Palworld",

    -- Server-side chat broadcast. FPalChatMessage: Sender / Message / Category.
    -- (The old per-player PalPlayerState:EnterChat_Receive RPC was removed by a patch.)
    chat = {
        hook = "/Script/Pal.PalGameStateInGame:BroadcastChatMessage",
        extract = function(self, param)
            local m = param:get()
            local cat = tonumber(m.Category) or 3
            local channel = (cat == 2) and "team" or "global"   -- 2 = Guild
            return m.Sender:ToString(), m.Message:ToString(), channel
        end,
    },

    death = {
        -- Verified by live UFunction dump: OnDeath does NOT exist; the real server-side
        -- player death is OnDeadPlayer_Server (fallbacks confirmed present on the build).
        candidates = {
            "/Script/Pal.PalPlayerCharacter:OnDeadPlayer_Server",
            "/Script/Pal.PalPlayerCharacter:OnDyingDeadEnd_Server",
            "/Script/Pal.PalCharacter:OnDeadCharacter",
        },
        extract = function(character)
            if character and character:IsValid() and character.PlayerState and character.PlayerState:IsValid() then
                local n = nameOf(character.PlayerState)
                return n, n
            end
        end,
    },

    players = function()
        local out = {}
        local list = FindAllOf("PalPlayerCharacter")
        if not list then return out end
        for _, P in ipairs(list) do
            if P and P:IsValid() and P.PlayerState and P.PlayerState:IsValid() then
                local n = nameOf(P.PlayerState)
                if n and n ~= "" then out[#out+1] = { gameId = n, name = n } end
            end
        end
        return out
    end,

    actions = {
        getPlayerLocation = function(args)
            local P = findByGameId(tostring(args.gameId or (args.player and args.player.gameId) or ""))
            if not P then return { x = 0, y = 0, z = 0, dimension = "0" } end
            local loc = P:K2_GetActorLocation()
            return { x = loc.X, y = loc.Y, z = loc.Z, dimension = "0" }
        end,

        giveItem = function(args)
            local gid = tostring(args.gameId or (args.player and args.player.gameId) or "")
            local itemId = tostring(args.name or args.item or "")
            local qty = tonumber(args.amount) or 1
            local P = findByGameId(gid)
            if not P then return false, "player not online" end
            local inv = P.PlayerState:GetInventoryData()
            if not inv or not inv:IsValid() then return false, "no inventory" end
            inv:RequestAddItem(FName(itemId), qty, false)
            return true, string.format("gave %d x %s", qty, itemId)
        end,

        teleportPlayer = function(args)
            local P = findByGameId(tostring(args.gameId or (args.player and args.player.gameId) or ""))
            if not P then return false, "player not online" end
            local x, y, z = tonumber(args.x) or 0, tonumber(args.y) or 0, tonumber(args.z) or 0
            P:K2_SetActorLocation({ X = x, Y = y, Z = z }, false, {}, true)
            return true, "teleported"
        end,
    },
}
