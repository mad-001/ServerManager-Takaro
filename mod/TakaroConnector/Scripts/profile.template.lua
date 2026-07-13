-- PROFILE TEMPLATE — copy to profile.lua and fill in the game-specific names.
-- Onboarding a new Unreal Engine game is ~this file, nothing else. Every field is
-- OPTIONAL: leave a field out and the universal core auto-detects it or falls back.
--
-- The universal core (main.lua) already handles, with NO help from you:
--   • join / leave      (roster diff — you just implement players())
--   • player-death      (if players() returns a `deaths` counter per player)
--   • the Takaro websocket, identify, ping/pong, testReachability, getPlayers.
--
-- You typically only need: chat (a hook + field extraction) and players().

return {
    name = "My UE Game",

    -- CHAT: which UFunction to hook, and how to pull (name, message, channel) from it.
    -- extract(self, param, ...) receives the hook args; return name, message, channel.
    chat = {
        hook = "/Script/MyGame.MyGameState:BroadcastChatMessage",
        extract = function(self, param)
            local m = param:get()
            return m.Sender:ToString(), m.Message:ToString(), "global"
        end,
        -- optional: derive a stable gameId for the speaker (defaults to their name)
        -- gameId = function(self, param) return param:get().SenderId:ToString() end,
    },

    -- DEATH (optional): only needed if you can't surface deaths via players().deaths.
    -- death = {
    --     hook = "/Script/MyGame.MyCharacter:OnDeath",
    --     extract = function(character) return character.PlayerState.PlayerName:ToString() end,
    -- },

    -- PLAYERS: enumerate the online roster. Drives join/leave, getPlayers, and (via the
    -- optional `deaths` field) death events. Return an array of:
    --   { gameId=<stable string>, name=<string>, steamId=<optional>, deaths=<optional> }
    players = function()
        local out = {}
        -- local GI = FindFirstOf("MyGameInstance")
        -- ... iterate GI.GameState.PlayerArray ...
        return out
    end,

    -- ACTIONS: handlers for the admin actions Takaro sends. Each gets `args` and returns
    -- either a result table, or (ok, message). Anything you don't implement simply
    -- reports "not implemented" back to Takaro. Common actions:
    --   sendMessage{message}  sendMessageToPlayer{gameId,message}  kickPlayer{gameId}
    --   banPlayer{gameId,reason}  giveItem{gameId,name,amount}  teleportPlayer{gameId,x,y,z}
    --   getPlayerLocation{gameId} -> {x,y,z}   getPlayerInventory{gameId} -> [{name,code,amount}]
    --   executeCommand{command}
    actions = {
        -- sendMessage = function(args) ... return true, "sent" end,
    },

    -- optional: run once after hooks install; gets the core table TC.
    -- init = function(TC) end,
}
