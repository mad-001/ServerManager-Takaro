-- Longvinter profile for ServerManager-Takaro
-- Engine: Unreal Engine 4. A /Script/Longvinter.* namespace exists (SteamIdComponent,
-- LongvinterFunctionsCPP, etc.), but chat/death/player logic is Blueprint — the gameplay
-- hooks live under /Game/..., not /Script/Longvinter.LongvinterGameState (which doesn't exist).
-- Hook paths VERIFIED via a live 250k-UObject dump on a running dedicated server (tester).
--   death        : /Game/ThirdPersonCPP/Blueprints/ThirdPersonCharacter.ThirdPersonCharacter_C:DeathServer
--   chat broadcast: /Game/Blueprints/Server/GM_Longvinter.GM_Longvinter_C:NewGlobatChatMessage  [sic]
-- CHAT HOOK CONFIRMED (tester, 2026-07-27 01:39 UTC): the GameMode NewGlobatChatMessage
-- broadcast DOES fire server-side and produces a real chat-message event. We hook THAT (not
-- the client-side ChatComponent receive). Because sendMessage also calls it, our own messages
-- would echo — the universal echo-guard in main.lua (records outbound sends, drops the echo)
-- handles that, so hooking the broadcast is safe. It is a SINGLE-FString broadcast: one param,
-- the whole (possibly pre-formatted) line, no sender field.

local function str(v) local o, r = pcall(function() return v:ToString() end); return o and r or nil end

-- The broadcast carries one FString and no sender. Pull the string robustly; if the server
-- pre-formats a real player's line as "Name: message", split it — otherwise emit with no
-- sender (the core then sends an empty player rather than garbage).
-- PROVISIONAL: the "Name: message" split is unconfirmed until a live player can chat; our own
-- messages never reach here (echo-guard), so this only matters for real player chat.
local function extractChat(self, param, channel)
    local text
    local ok1, v1 = pcall(function() return param:get() end)
    if ok1 and v1 then text = str(v1) or (type(v1) == "string" and v1) or nil end
    if not text then local ok2, v2 = pcall(function() return param:ToString() end); if ok2 then text = v2 end end
    if not text and type(param) == "string" then text = param end
    text = text or ""
    local name, msg = text:match("^(.-):%s(.*)$")
    if name and msg and #name > 0 and #name <= 24 then
        return name, msg, channel or "global"
    end
    return nil, text, channel or "global"
end


-- Resolve a player's live Character by Takaro gameId (== PlayerNamePrivate from the roster).
local function findChar(id)
    id = tostring(id or "")
    local ok, list = pcall(function() return FindAllOf("Character") end)
    if not ok or not list then return nil end
    for _, c in ipairs(list) do
        if c and c:IsValid() then
            local nm; pcall(function() nm = c.PlayerState.PlayerNamePrivate:ToString() end)
            if nm == id then return c end
        end
    end
    return nil
end
local function argId(a) return tostring(a.gameId or (a.player and a.player.gameId) or a.name or "") end

-- UFunction reflection (READ-ONLY, never calls). A wrong-signature call hard-hangs the game
-- thread (BUG 4), so giveItem reflects before calling and refuses unknown shapes.
local function reflectParams(func)
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
local function findFuncOnClass(obj, fname)
    local target = nil
    pcall(function()
        obj:GetClass():ForEachFunction(function(f)
            local n; pcall(function() n = f:GetFName():ToString() end)
            if n == fname then target = f end
        end)
    end)
    return target
end

local function gm()
    local g = FindFirstOf("GM_Longvinter_C"); if g and g:IsValid() then return g end; return nil
end

return {
    name = "Longvinter",

    chat = {
        -- CONFIRMED firing server-side (tester 01:39 UTC). Self-echo handled by main.lua's guard.
        hook = "/Game/Blueprints/Server/GM_Longvinter.GM_Longvinter_C:NewGlobatChatMessage",
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
        -- Actions below use tester-dumped Longvinter functions (250k UObject dump) —
        -- signatures BEST-EFFORT, please confirm live via an injected ipc/req.
        getPlayerLocation = function(args)
            local c = findChar(argId(args)); if not c then return { x=0, y=0, z=0, dimension="0" } end
            local loc = c:K2_GetActorLocation(); return { x=loc.X, y=loc.Y, z=loc.Z, dimension="0" }
        end,
        teleportPlayer = function(args)
            local c = findChar(argId(args)); if not c then return false, "offline" end
            local v = { X=tonumber(args.x) or 0, Y=tonumber(args.y) or 0, Z=tonumber(args.z) or 0 }
            -- Don't trust pcall: ServerAdminTeleport can return without erroring yet not move
            -- the player (wrong signature / needs extra state). Verify by reading the location
            -- back after each attempt — success only if the player actually moved.
            local function loc() local ok, l = pcall(function() return c:K2_GetActorLocation() end); return ok and l or nil end
            local function moved(a, b)
                if not a or not b then return false end
                local dx=(a.X or 0)-(b.X or 0); local dy=(a.Y or 0)-(b.Y or 0); local dz=(a.Z or 0)-(b.Z or 0)
                return (dx*dx + dy*dy + dz*dz) > 1.0          -- moved > ~1 unit
            end
            local before = loc()
            -- 1) game's admin teleport (handles collision/ClearSpaceForTeleport/replication)
            pcall(function() c:ServerAdminTeleport(v) end)
            if moved(before, loc()) then return true, "teleported (ServerAdminTeleport)" end
            -- 2) engine fallback
            pcall(function() c:K2_SetActorLocation(v, false, {}, true) end)
            if moved(before, loc()) then return true, "teleported (K2_SetActorLocation)" end
            return false, "teleport had no effect (neither ServerAdminTeleport nor K2_SetActorLocation moved the player)"
        end,
        giveItem = function(args)
            local c = findChar(argId(args)); if not c then return false, "offline" end
            local pc; pcall(function() pc = c.Controller end)  -- PC_Longvinter_C
            if not pc or not pc:IsValid() then return false, "no controller" end
            local item = tostring(args.name or args.item or ""); local qty = tonumber(args.amount) or 1
            -- BUG 4: AdminGiveItemsServer's signature was a guess, and a wrong-arity/type call
            -- HARD-HANGS the game thread (pcall can't catch it). Reflect the params first and
            -- ONLY call if the shape is one we can safely fill; otherwise report the discovered
            -- signature so it can be pinned, without ever risking the wedge.
            local func = findFuncOnClass(pc, "AdminGiveItemsServer")
            local params = func and reflectParams(func) or nil
            if not params then
                return false, "giveItem: could not reflect AdminGiveItemsServer — not blind-calling (would risk a game-thread wedge). Run the 'reflect' action to get its signature."
            end
            local inp = {}
            for _, p in ipairs(params) do if not p.ret then inp[#inp+1] = p end end
            local t1 = inp[1] and inp[1].type
            local t2 = inp[2] and inp[2].type
            -- known-safe shape: (Name|Str item, Int quantity)
            if #inp == 2 and (t1 == "NameProperty" or t1 == "StrProperty") and t2 == "IntProperty" then
                local a1 = (t1 == "NameProperty") and FName(item) or item
                local ok, err = pcall(function() pc:AdminGiveItemsServer(a1, qty) end)
                if not ok then return false, "AdminGiveItemsServer failed: " .. tostring(err) end
                return true, string.format("gave %d x %s", qty, item)
            end
            local desc = {}
            for _, p in ipairs(inp) do desc[#desc+1] = (p.name or "?") .. ":" .. (p.type or "?") end
            return false, "giveItem held (BUG 4): AdminGiveItemsServer signature is [" ..
                table.concat(desc, ", ") .. "] — map these params before calling (not blind-calling to avoid a wedge)"
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
