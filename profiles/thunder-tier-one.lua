-- Thunder Tier One profile for ServerManager-Takaro
-- Engine: ue4. Chat is auto-discovered at runtime; confirm on your server.
--
-- Player join/leave, roster, getPlayers and death work with NO changes here — the core
-- uses the engine-standard FindAllOf("PlayerState") roster. Chat is auto-discovered at
-- runtime by probing this game's real GameState/PlayerController classes for common chat
-- functions. If chat does not appear in Takaro, dump this game's UFunctions with UE4SS
-- (Objects dumper) and set an explicit hook below.

return {
    name = "Thunder Tier One",
    -- To pin chat manually, uncomment and set the real UFunction path + field names:
    -- chat = {
    --     hook = "/Script/<Module>.<GameStateClass>:<ChatFunction>",
    --     extract = function(self, param)
    --         local m = param:get()
    --         return m.Sender:ToString(), m.Message:ToString(), "global"
    --     end,
    -- },
}
