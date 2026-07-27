-- dump_hooks.lua — one-shot UE4SS Lua helper to find a game's chat/death/player UFunctions.
-- Drop as a UE4SS Lua mod (its own folder + enabled), boot the dedicated server, and it
-- writes ue4ss/hook_candidates.txt listing every UFunction whose full name matches
-- chat/death/message/join patterns — including Blueprint (/Game/...) functions that the
-- pattern-sweep misses. Paste the right paths into the game's profile.lua.
-- (Generalizes the method that found Longvinter's ChatComponent_C:NewGlobalChatMessage
--  and Palworld's OnDeadPlayer_Server.)
local OUT = "ue4ss/hook_candidates.txt"
local PAT = { "chat", "message", "death", "dead", "die", "kill", "join", "leave",
              "connect", "spawn", "login", "logout", "say", "broadcast" }
local function matches(n) local l=n:lower(); for _,p in ipairs(PAT) do if l:find(p,1,true) then return true end end return false end

ExecuteWithDelay(8000, function()
    local seen, out = {}, {}
    local ok, err = pcall(function()
        ForEachUObject(function(obj)
            if not obj or not obj:IsValid() then return end
            local isfn = false
            pcall(function() isfn = obj:GetClass():GetFName():ToString() == "Function" end)
            if not isfn then return end
            local full; pcall(function() full = obj:GetFullName() end)
            if not full then return end
            -- GetFullName is like "Function /Game/.../Foo_C:Bar" — strip the leading word
            local path = full:match("%s(/[%w_%./]+:%S+)$") or full
            if matches(path) and not seen[path] then seen[path]=true; out[#out+1]=path end
        end)
    end)
    table.sort(out)
    local f = io.open(OUT, "w")
    if f then
        f:write("# chat/death/player UFunction candidates (grep and paste into profile.lua)\n")
        f:write("# scan ok=" .. tostring(ok) .. (err and (" err="..tostring(err)) or "") .. " count=" .. #out .. "\n\n")
        for _,p in ipairs(out) do f:write(p.."\n") end
        f:close()
    end
    print("[dump_hooks] wrote " .. #out .. " candidates to " .. OUT)
end)
