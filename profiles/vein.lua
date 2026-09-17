-- VEIN profile for ServerManager-Takaro.
-- Engine: Unreal Engine 5.6. Steam dedicated-server appid 2131400 (anonymous).
-- Server exe: Vein/Binaries/Win64/VeinServer-Win64-Test.exe.
--
-- IMPORTANT — UE4SS on UE5.6: the generic bundled UE4SS (3.0.1-1088) FAILS VEIN's AOB
-- scan (StaticConstructObject_Internal -> "PS scan timed out"), so the Lua side won't load.
-- Deploy UE4SS experimental-latest (3.0.1-1136) + the official Vein CustomGameConfig
-- (UE4SS_Signatures/StaticConstructObject.lua + FText_Constructor.lua, MemberVariableLayout.ini,
-- VTableLayout.ini, UE4SS-settings.ini) from the UE4SS release's zCustomGameConfigs.zip.
-- Verified live: server identifies with Takaro; chat/death auto-detect at runtime.
return {
    name = "VEIN",
}
