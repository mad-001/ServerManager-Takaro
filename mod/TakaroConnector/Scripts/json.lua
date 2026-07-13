-- Minimal JSON encode/decode (pure Lua, no deps). Ported from the VoxelTurf connector.
local M = {}

local function encVal(v, d)
    d = d or 0
    if d > 12 then return '"..."' end
    local t = type(v)
    if t == "nil" then return "null"
    elseif t == "boolean" then return v and "true" or "false"
    elseif t == "number" then
        if v ~= v or v == math.huge or v == -math.huge then return "null" end
        if v == math.floor(v) and math.abs(v) < 1e15 then return string.format("%d", v) end
        return tostring(v)
    elseif t == "string" then
        v = v:gsub('\\','\\\\'):gsub('"','\\"'):gsub('\n','\\n'):gsub('\r','\\r'):gsub('\t','\\t'):gsub('%z','')
        return '"' .. v .. '"'
    elseif t == "table" then
        local n, isArr = 0, true
        for k,_ in pairs(v) do
            n = n + 1
            if type(k) ~= "number" or k ~= math.floor(k) or k < 1 then isArr = false end
        end
        if n == 0 then return "{}" end
        if isArr and n == #v then
            local parts = {}
            for i,x in ipairs(v) do parts[i] = encVal(x, d+1) end
            return "[" .. table.concat(parts, ",") .. "]"
        else
            local parts = {}
            for k,x in pairs(v) do
                if type(k) == "string" then
                    parts[#parts+1] = '"' .. k:gsub('\\','\\\\'):gsub('"','\\"') .. '":' .. encVal(x, d+1)
                end
            end
            return "{" .. table.concat(parts, ",") .. "}"
        end
    else return '"[' .. t .. ']"' end
end
function M.encode(v) return encVal(v, 0) end

local function skip(s,i) while i<=#s and s:sub(i,i):match("%s") do i=i+1 end return i end
local function pStr(s,i)
    i=i+1; local r={}
    while i<=#s do
        local c=s:sub(i,i)
        if c=='"' then return table.concat(r), i+1
        elseif c=='\\' then
            i=i+1; local e=s:sub(i,i)
            if e=='"' then r[#r+1]='"' elseif e=='\\' then r[#r+1]='\\'
            elseif e=='n' then r[#r+1]='\n' elseif e=='r' then r[#r+1]='\r'
            elseif e=='t' then r[#r+1]='\t' elseif e=='/' then r[#r+1]='/'
            else r[#r+1]=e end
        else r[#r+1]=c end
        i=i+1
    end
    return table.concat(r), i
end
local pVal
local function pObj(s,i)
    i=i+1; local r={}; i=skip(s,i)
    if s:sub(i,i)=='}' then return r,i+1 end
    while i<=#s do
        i=skip(s,i); if s:sub(i,i)~='"' then break end
        local k,ni=pStr(s,i); i=ni; i=skip(s,i)
        if s:sub(i,i)==':' then i=i+1 end; i=skip(s,i)
        local val,vi=pVal(s,i); r[k]=val; i=vi; i=skip(s,i)
        local ch=s:sub(i,i)
        if ch==',' then i=i+1 elseif ch=='}' then return r,i+1 else break end
    end
    return r,i
end
local function pArr(s,i)
    i=i+1; local r={}; i=skip(s,i)
    if s:sub(i,i)==']' then return r,i+1 end
    while i<=#s do
        i=skip(s,i); local val,vi=pVal(s,i); r[#r+1]=val; i=vi; i=skip(s,i)
        local ch=s:sub(i,i)
        if ch==',' then i=i+1 elseif ch==']' then return r,i+1 else break end
    end
    return r,i
end
pVal = function(s,i)
    i=skip(s,i); local c=s:sub(i,i)
    if c=='"' then return pStr(s,i)
    elseif c=='{' then return pObj(s,i)
    elseif c=='[' then return pArr(s,i)
    elseif s:sub(i,i+3)=='true'  then return true, i+4
    elseif s:sub(i,i+4)=='false' then return false,i+5
    elseif s:sub(i,i+3)=='null'  then return nil,  i+4
    else
        local ns=s:match("^-?%d+%.?%d*[eE]?[+-]?%d*",i)
        if ns then return tonumber(ns), i+#ns end
    end
    return nil,i+1
end
function M.decode(s)
    if not s or s=="" then return nil end
    local ok,res=pcall(function() local v,_=pVal(s,1); return v end)
    return ok and res or nil
end

return M
