# Fear3CabbyCodes

A cheat panel for **F.E.A.R. 3** (Steam, PC). Pause the game during a mission and a small panel
appears beside the pause menu:

- **God mode** - you take no damage and cannot die. Uses the game's own "indestructible" state
  (the one its cutscenes use), applied to your character and re-applied after a checkpoint reload
  or a respawn.
- **Infinite slo-mo** - the slow-mo meter is held full, so it never runs out. Point Man only; the
  box is greyed out when you play as Fettel, who has no slow-mo.
- **Infinite possession** - a body you possess never times out: the possession timer is reset every
  frame with the game's own `ResetPossessionTimer`. Fettel only; greyed out for Point Man.
- **Infinite ammo** - every weapon you carry, the equipped one and your grenades included, is
  filled up, and the counts never go down: each shot is put straight back before the HUD sees it.
- **One hit kills** - anything you damage takes a lethal amount instead. Enemies with scripted
  invulnerability phases keep their phases; everything else dies on the first hit.

Tick what you want and unpause. The switches keep their state for the session (start-up defaults
live in the ini). **F7** hides or shows the panel while the game is paused.

Works on Windows and on Linux/Proton with no launch options. Direct3D 11 (the default) and
Direct3D 9 (`-d3d9` in `options.cfg`) are both supported. It coexists with
[Fear3ChallengeGrant](https://github.com/SirCabby/Fear3ChallengeGrant) (which proxies
`binkw32.dll`) and [Fear3TimeManager](https://github.com/SirCabby/Fear3TimeManager) (`fmodex.dll`):
this mod proxies `steam_api.dll`.

## Install

1. Open your F.E.A.R. 3 folder (the one containing `F.E.A.R. 3.exe`).
2. Rename the existing `steam_api.dll` to `steam_api_orig.dll`.
3. Copy the mod's `steam_api.dll` in beside it.

Steam's *Verify integrity of game files* puts the stock DLL back; just repeat step 3 if that happens.
To uninstall, delete the mod's `steam_api.dll` and rename `steam_api_orig.dll` back.

## Please read before using

- The cheats change what the game scores you on (kills, damage taken, ammo). Nothing is written to
  your saves by the mod itself, but a save made while cheating records the game state as it was.
- God mode does not stop scripted deaths (falling out of the world, a scripted kill) - only damage.
- One hit kills also make your own explosives lethal to anyone but you; with god mode off, your own
  grenade at your feet is still your own grenade at your feet.

## Config

`Fear3CabbyCodes.ini` is created next to the DLL on first run:

| Key | Default | Meaning |
| --- | --- | --- |
| `ToggleKey` | `0x76` (F7) | virtual-key code that hides/shows the panel while paused |
| `PlayerIndex` | `0` | local player slot the cheats apply to |
| `GodMode`, `InfiniteSlowMo`, `InfinitePossession`, `InfiniteAmmo`, `OneHitKills` | `0` | cheats switched on when the game starts |
| `OneHitDamage` | `1000000` | the damage every hit you land is raised to |
| `AlwaysShow` | `0` | debug: draw the panel outside the pause menu too |
| `Trace` | `0` | verbose diagnostics in `Fear3CabbyCodes.log` |
| `Disable` | | comma list of subsystems to turn off: `overlay,dispatch,game` |

`Fear3CabbyCodes.log` beside the DLL records what the mod found and did; attach it when reporting a
problem.

## How it works

The mod ships as a proxy `steam_api.dll` (every export of the game's Steam API DLL is forwarded
untouched through generated jump thunks), so it loads before the game starts and needs no injector.
It locates what it needs at runtime by signature and RTTI - never by fixed address; the
Steam-protected executable is never modified - and drives the engine through its own interfaces:
the game's "make this character indestructible" routine for god mode, the slow-mo component's
meter for slo-mo, the possession component's own timer reset for possession, the ammo component's setters (put back from a hook on the weapon-fire message)
for ammo, and the damage message's amount (edited in a hook on its delivery) for one hit kills.
The panel is Dear ImGui drawn from a hook on the swap chain's `Present`. See `CLAUDE.md` for the
reverse-engineering record.

## Building from source

Linux with mingw-w64 (`i686-w64-mingw32-g++`); no Windows or MSVC needed.

```sh
cp config.mk.example config.mk   # set GAME_DIR
make                             # build/steam_api.dll
make install                     # deploy into GAME_DIR (renames the stock DLL once)
make rev 1.1.0                   # set the version (VERSION file, baked into the DLL)
make package                     # dist/Fear3CabbyCodes_v<version>.zip
make proxy                       # regenerate the export list from the stock DLL
```

Dear ImGui (MIT) is vendored under `contrib/imgui`. Licensed under the GPL-3.0; see `LICENSE`.
