[size=6][b]Fear3CabbyCodes[/b][/size]

A [b]cheat panel[/b] for F.E.A.R. 3: pause the game during a mission and tick what you want - God mode, Infinite slo-mo, Infinite possession, Infinite ammo, One hit kills.

This mod is open source! Check it out at [url=https://github.com/SirCabby/Fear3CabbyCodes]https://github.com/SirCabby/Fear3CabbyCodes[/url]

[size=5][b]What it does[/b][/size]
[list]
[*][b]God mode[/b] - no damage, no dying. The game's own "indestructible" state, re-applied after checkpoint reloads.
[*][b]Infinite slo-mo[/b] - the meter stays full. Point Man only (Fettel has no slo-mo; the box is greyed out).
[*][b]Infinite possession[/b] - a possessed body never times out. Fettel only.
[*][b]Infinite ammo[/b] - every weapon you carry, grenades included, is filled up and the counts never go down.
[*][b]One hit kills[/b] - everything you damage dies on the first hit (scripted invulnerability phases are kept).
[*][b]F7[/b] hides or shows the panel while paused. Start-up defaults live in the ini.
[/list]

[size=5][b]Install[/b][/size]

Copy into your F.E.A.R. 3 folder (the one with F.E.A.R. 3.exe):
[list=1]
[*]Rename the stock [b]steam_api.dll[/b] to [b]steam_api_orig.dll[/b]
[*]Drop this mod's [b]steam_api.dll[/b] in its place
[/list]

That is the whole install, on [b]Windows and Linux/Proton alike[/b] - no launch options, no WINEDLLOVERRIDES, no ASI loader. It works side by side with Fear3ChallengeGrant and Fear3TimeManager.

To uninstall: delete steam_api.dll and rename steam_api_orig.dll back.

[size=5][b]Please read before using[/b][/size]
[list]
[*][b]The cheats change what the game scores you on.[/b] Nothing is written to your saves by the mod itself.
[*]God mode stops damage, not scripted deaths (falling out of the world, scripted kills).
[*][b]Steam's "Verify integrity of game files" removes the mod[/b] by restoring the stock DLL. Just re-copy the file.
[/list]

[size=5][b]Config[/b][/size]

[code]
ToggleKey      = 0x76   ; virtual-key code that hides/shows the panel while paused (0x76 = F7)
PlayerIndex    = 0      ; local player slot the cheats apply to
GodMode        = 0      ; cheats switched on when the game starts
InfiniteSlowMo = 0
InfinitePossession = 0
InfiniteAmmo   = 0
OneHitKills    = 0
OneHitDamage   = 1000000 ; damage every hit you land is raised to with One hit kills
[/code]

Lives in [b]Fear3CabbyCodes.ini[/b] next to F.E.A.R. 3.exe (created on first run). [b]Fear3CabbyCodes.log[/b] beside it records what the mod found and did - attach it when reporting a problem.
