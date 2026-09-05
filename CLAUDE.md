# Fear3CabbyCodes — project guide

A client-side mod for **F.E.A.R. 3** (Steam appid 21100, 32-bit `F.E.A.R. 3.exe`, Despair engine,
Steam CEG-protected), cross-built on Linux with mingw-w64. It draws a Dear ImGui panel over the
in-game pause menu with four switches: God mode, Infinite slo-mo, Infinite ammo, One hit kills.
`README.md` is the user-facing doc; this file records **what was reverse-engineered**. It is a
sibling of `../Fear3ChallengeGrant` (whose `CLAUDE.md` holds the shared discoveries in full:
registration patterns, class descriptors vs instances, the heap-scan instance finder, renderer
capture, gotchas) and `../Fear3TimeManager`; only what is specific to the cheats is repeated here.

## Build & deploy

```sh
make            # -> build/steam_api.dll   (config.mk sets GAME_DIR; gitignored)
make install    # rename stock steam_api.dll -> steam_api_orig.dll (once), deploy ours atomically
make uninstall  # restore the stock DLL
make rev X.Y.Z  # set the version;  make package -> dist/Fear3CabbyCodes_vX.Y.Z.zip
make proxy      # regenerate steam_api.def + src/proxy_exports.inc from the stock DLL's export table
python3 tools/find_regs.py --exe "$GAME_DIR/F.E.A.R. 3.exe" --name GetInventoryItems   # etc.
```

The log (`Fear3CabbyCodes.log`), ini (`Fear3CabbyCodes.ini`) and ImGui layout file sit beside the
DLL in the game folder. `Trace = 1` adds diagnostics; `AlwaysShow = 1` draws the panel outside the
pause menu (rendering test); `Disable = overlay,dispatch,game` bisects a fault.

## ⛔ Rules

- **Never patch `F.E.A.R. 3.exe` on disk** (CEG). Proxy DLL plus in-memory hooks only.
- **Never use absolute addresses.** Everything is found by signature at runtime from the method-name
  strings and verified against the bytes of the functions it is read from; the VAs in this file
  document build id 3576 (`ResVersion 16.00.20.0275`) only.
- **Game calls only on the main thread** (`dispatch.cpp`'s `PeekMessageA` hook, primary thread only).
  The receiver hooks run on whatever thread the game sends the message from and only touch atomics
  and the message.
- **Never call a game function on a guessed layout.** Every slot and field here came from the game's
  own code that uses it; `game.cpp` re-checks the accessor bytes per vtable before the first use.
- The user commits every repo himself — do not `git commit`/`push` unless asked.

## Why steam_api.dll
`binkw32.dll` (Fear3ChallengeGrant) and `fmodex.dll` (Fear3TimeManager) are taken, and two proxies
of one DLL cannot coexist. Of the exe's remaining imports only `steam_api.dll` ships with the game
and is not a Wine builtin (`d3d9`, `dinput8`, `xinput1_3`, `d3dcompiler_43`, `version`, `winmm`
would need `WINEDLLOVERRIDES` under Proton; the MSVC 9 runtime is SxS). The exe imports 24 of its
54 exports, all `__cdecl` C functions; `tools/gen_proxy.py --from-dll` emits thunks for **all 53
functions** (anything the game or CEG resolves with `GetProcAddress` at runtime is then covered too)
and a PE forwarder (`name = steam_api_orig.name DATA`, which GNU ld supports) for the one data
export, `g_pSteamClientGameServer`. CEG talks to the real DLL through the thunks; nothing is emulated.

## What the game does

### Players and characters
`GetPlayer(idx)` (`0x9EF2D0`, `__cdecl`, walks the static pair vector at `0x183BD3C..40` of
`{avatar handle, IPlayerComponent*}` and returns the component whose slot 19 index matches) is the
`E8` inside `GameScriptGame::HasPlayerStartedLevel` (`0x616950`: `... call GetPlayer; player->vtbl[40]()`).
`IPlayerComponent` (`PlayerComponent+0x44`, vtable `0x184301C`, 43 slots):

| slot | function | what |
| --- | --- | --- |
| 0 | `0x7E80E0` | the **IActorComponent** of the character the player controls (`[this+0x1C]`, guarded by the handle at `+0x18`) |
| 1 | `0x9E0D60` | the player's own avatar object (handle at `+0x20` -> `[+8]-0x18`) |
| 19 | `0xC6A950` | player index (`[this+0x10]`) |
| 40 | `0xDAD890` | has started the level (`[this+0xED]`) |

`IActorComponent` (type id `0x1833298`, registered as "IActorComponent"): slot 12 (`[vtbl+0x30]`)
returns the game object, slot 16 (`[vtbl+0x40]`) is `SetIndestructible(bool on, bool allowDamage)`.
Both come from two helpers the mod locates by shape:

- **GetPlayerObject** `0x52D940`: `GetPlayer(idx)->vtbl[0]()->vtbl[12]()`.
- **SetPlayerIndestructible** `0x437571` (called with `[esi+0xA8]` = player index): `obj =
  GetPlayerObject(idx); comp = obj->GetComponent(IActorComponent); if (comp) comp->vtbl[16](on, 0);
  else { IndestructibleMessage m(on, 0); obj->Send(&m); }`. The mod does exactly this for god mode.
  The retail `GameSettings::TogglePlayerIndestructible` (`0x651CE0`, script class `GameSettings` at
  `0x18A3E10`, registered with a third code shape `C7 46 10 <name> C7 46 14 <owner>`) is the same
  thing plus a per-player flag table; `TogglePlayerInfiniteSlowMo/Energy/Possession` are stubs
  (`ret`) in this build.

### Game objects, components, messages
An object handle (what script wrappers and inventory items store) is a control block
`{vtable, refcount, target}` with `target = object+0x18` (`0x18` alone = null). On the object:
`+0x4` is the component container (`vtbl[0](typeId)` = `GetComponent`), `+0x14` a second one
(`vtbl[2](typeId, std::vector<IComponent*>&)` = all components of a type; the vector is
`{begin, end, cap}` and the wrapper frees it with `free(begin, cap-begin, heap())`, the two
`__cdecl` helpers `0x48A210`/`0x8A0E30` at the end of `GetInventoryItems`), `+0x60` the message
router. **`Object::Send(msg)` = `0x668D90`** (`add ecx,0x60; jmp 0x5B8A40`): the router looks the
message type up (`msg->vtbl[1]()`), and for each matching record `{type, ?, impl, component, fn,
adjust, flags}` calls `impl->vtbl[0](component, fn, adjust, msg, &flags)` — `impl` being the
**global `ReflectionReceiver::Implementation<Class, Message>` object** of that pair (one per pair,
its vtable pointer set lazily by a guarded initialiser `or [guard],1; mov [global], vtable`), whose
one virtual is the shared dispatcher `0x94B130` (`ecx = component + adjust; push msg; call fn`;
`ret 0x14`). Swapping that global's vtable pointer for our own `{hook, ...}` is the mod's hook
point: every delivery of that message to that class passes through the hook first, on any thread,
with the complete component pointer and the message. Components register their receivers per
instance (`AmmoComponent` ctor: `0xE074F0(router, &out, this, 0xA8B6B0 /*handler*/, 0)`), so the
handler addresses are only documentation:

| receiver | vtable | global | guard | handler |
| --- | --- | --- | --- | --- |
| `AmmoComponent <- WeaponFireMessage` | `0x1452A94` | `0x17743F8` | `0x1779B50` | `0xA8B6B0` |
| `BasicHitPointComponent <- DamageMessage` | `0x1457BDC` | `0x1774400` | `0x176B7C0` | `0xCFD3D0` |
| `HitPointControllerComponent <- DamageMessage` | `0x1454F2C` | `0x175FB44` | `0x1762F24` | `0xE9FFB0` |
| `LocationalHitPointControllerComponent <- DamageMessage` | `0x144DAD0` | `0x1765BA8` | `0x177B664` | `0x86B400` |

A message is `{vtable, fields...}` (the base class has only the vtable). **IndestructibleMessage**
(vtable `0x1866874`, type `0x1866898`) is `{vtable, u8 indestructible, u8 allowDamage}`; its
constructor `0xE88610(this, on, allow)` is what the mod calls (verified byte for byte).
`BasicHitPointComponent::OnIndestructible` (`0x67DCE0`) sets `+0xDE` (indestructible) and
`+0xDF` (no damage at all); `SetHitPoints` (`0xE91530`) then floors the value, and the damage
modifier (`0xD78210`) caps the absorbable damage. `HitPointControllerComponent::OnIndestructible`
(`0xDBF280`) sets `+0xF1` and, for `allowDamage == false`, **unregisters its DamageMessage receiver**
(`0xC98640` re-registers it on the way back).

**DamageMessage** (vtable `0x184F298`, ctor `0x91A8D0`) is `{vtable, DamageInfo}`; the info
(constructor `0xD77DF0`, `0x64` bytes) starts at message `+4`:

| info | field |
| --- | --- |
| `+0x0` | flags byte; bit 1 = kill outright (the handlers then use the component's max HP) |
| `+0x4` | `DamageInfo::DamageSourceType` (Default, Splash, DamageOverTime, Melee, Crushing) |
| `+0x8` | **float amount** |
| `+0xC` | dealer object handle |
| `+0x10` | **dealer player index** (-1 = not a player; `HitPointControllerComponent` stores it at `+0xD8`, which `GetLastDamagePlayerIndexFromObject` reads through interface slot 15) |
| `+0x28/+0x2C` | vector of 12-byte records (damage types) |
| `+0x48` | embedded `DamageDealer` (vtable `0x183D560`) |

`HitPointControllerComponent::OnDamage` multiplies the amount by `0xA34620(info)` (difficulty and
damage-type factors), zeroes it for invulnerable priority buckets (`bucket+0x26`, set by
`SetHitPointPriorityInvulnerable`), and hands it down the bucket list; the same message object
visits every receiver on the target, so editing the amount once in the first hook is enough.

### Ammo (`IAmmoComponent` = `AmmoComponent+0x44`, vtable `0x18541CC`; type id `0x1853FC0`)
Slots, relative to the interface pointer (`SetCurrentTotalAmmoPercent` uses 17 and the setters):

| slot | function | what |
| --- | --- | --- |
| 5 | `0x95C920` | max carried, 10000 when the carried ammo is infinite |
| 7 | `0x9D7760` | `[+0xBC] * [+0xAC]` = nMaxClipsCarried * nAmmoPerClip |
| 10 | `0x621970` | **SetAmmoInGun(n)** (clamps to max; `0xAC6AB0` on the complete object, then a status update) |
| 11 | `0xE4F720` | `[+0xB8]` = nCurrentAmmoInGun, raw |
| 12 | `0xB7FFB0` | ammo in gun through an ammo modifier when one is linked (`+0x32C/+0x330`) - the HUD's `m_nCurrentWeaponAmmo` |
| 13 | `0x724460` | carried: the shared **AmmoPoolComponent** amount (`+0x334/+0x338`, type index `+0x33C`) plus the local `[+0xC4]` |
| 14 | `0x9E90A0` | `[+0xB0] * [+0xAC]` = nMaxClipsInGun * nAmmoPerClip |
| 17 | `0x87BC90` | SetCurrentTotalAmmoPercent(float) |
| 30 | `0x9FE1E0` | bCarriesInfiniteAmmo (`[+0xA4]` bit 1; bit 0x400 = bUseAmmoPool) |
| 39 | `0xCF9C70` | **SetAmmoCarried(n)** (`0xEA7240`: the local field only) |

`AmmoComponent::OnWeaponFire` (`0xA8B6B0`) does `SetAmmoInGun(gun - nPrimaryFireAmmoToConsume
[+0x10C])` and starts an auto-reload if the gun is left short — so the mod's weapon-fire hook
restores the gun through slot 10 right after the original handler, before the HUD reads it
(`FEARWeaponHUDHelper::UpdateWeaponAmmo` `0xD640E0` polls slot 12 of the weapon at `helper+0x60` and
slot 11 of the grenade at `helper+0x64`). Grenades are an inventory item with an ammo component
like any weapon; their count is the in-gun value.

Inventory (`GameScriptGame::GetInventoryItems` `0xD621C0`): all `IInventoryListComponent`s
(type `0x1835D50`) of the object, each with `vtbl[52]()` groups, `vtbl[53](i)` group,
`group->vtbl[60]()` items, `group->vtbl[36](j)` item, and the item's object handle at `item+0x18`.

### Slow-mo (`ISlowMoComponent` = `SlowMoComponent+0`, vtable `0x184B460`; type id `0x184B890`)
`RefillSlowMo` calls slot 3 (`fld [ecx+0x6C]` = max) then slot 4 (`Set(float)`: clamps to max,
stores `+0x74`, and broadcasts a recharged message (`0x184B3BC`) on the empty->full transition);
`GetSlowMoPercentRemaining` divides slot 5 (`fld [ecx+0x74]` = current) by slot 3. Slot 11
(`0x87B190`) is "enough to activate" (`+0x74 > +0x68`). The mod writes `+0x74 = +0x6C` every tick
instead of calling the setter, because while the meter drains the setter would broadcast the
recharge message every frame. Fettel's avatar has no `ISlowMoComponent`; the panel greys the
switch out when `GetComponent` returns null.

### Possession (Fettel)
`GameScriptPlayer` (descriptor `0x1870590`) registers its methods with a fourth shape,
`B8 <impl> 50 68 <name> E8 rel32 83 C4 0C 50 8B CD E8`: `ResetPossessionTimer(idx)` `0x50ADD0`,
`PauseCurrentPossession(idx, on)` `0x547DA0`, `InhibitCurrentPossessionEnding(idx, on)` `0xADBC60`,
`GetMaxPossessionTime(h)` `0xA3B910`, `GetPossessionDelay(h)` `0x7E15E0`, `CanPlayerPossess(idx)`
`0xD8BDE0`, `GetPossessingPlayerIndex(h)` `0x4FC780`. They fetch the **IPossessionComponent** (type
`0x1837F08`, on Fettel's own object; the script host's slot 51 looks it up "on the avatar") and call:

| slot | IPossessionComponent (`PossessionComponent+0x44`, vtable `0x18FBB24`) |
| --- | --- |
| 1 | state (`[+0x1A4]`; **4 = possessing**, 0 = idle, 1/2 = starting) |
| 3 | the body's IPossessedComponent (`[+0x19C]` handle, `[+0x1A0]` interface) |
| 5 | begin possession of an object handle |
| 6 | can possess |
| 10 | SetPaused(bool) (`[+0x21A]`; resets the remaining time) |
| 11 | Pause(bool, requester) (`[+0x218]`, requester list at `[+0x298]`; mirrors to the body's slot 17) |
| 12 | **ResetTimer**: if possessing, `remaining(+0x1B4, double) = fPossessionTime(+0xAC)` and the body's slot 18 |
| 13 | InhibitEnding(bool, requester) (`[+0x219]`, list at `[+0x308]`; mirrors to the body's slot 19) |

**IPossessedComponent** (type `0x1837EA0`, `PossessedComponent+0x44`, vtable `0x1872F34`): slot 1
possessed flag (`[+0x104]`), 3 activate(bool) (`[+0x105]`; stores the game clock `0x17EC280` in
`[+0x14C]` on activation), 5 started-at getter, 7 the possessor's object (`[+0x100]` handle), 8
possessing player index (`[+0x178]`), 13 SetPaused (`[+0x106]`; resets remaining), 14/15 delay
(`[+0x15C]`), 18 **reset** (`remaining(+0x154, double) = max(+0xA0)`), 20 max time, 21 SetMax
(rescales remaining). Reflected fields: `fPossessionTime` at `PossessionComponent+0xF0`,
`possessionTimeRemaining` `+0x1F8`; `possessionTimeRemaining` at `PossessedComponent+0x198`, the HUD's
`possessionPctTimeRemainingVar` = `[+0x198] / [+0xE4]` (`0xC93930`). Neither "remaining" is decremented
in place anywhere in `.text` and the expiry code was not found (it is not reached through the
cached body interface); the mod therefore calls ResetTimer every tick while possessing **and** holds
the body's started-at clock at now, covering both a decremented and a clock-measured countdown.
`GameSettings::TogglePlayerInfinitePossession` is a stub in this build.

### Pause / in-level gating
As in the siblings: `HasPlayerStartedLevel(player)` (called) and `MenuMgr::IsPauseMenuShowing`
(slot 69 on the `+4` subobject of the heap-scanned MenuMgr).

## Tooling notes
- Ghidra headless (`/opt/ghidra/support/analyzeHeadless`, `MAXMEM=12G`) analyses the exe in ~11
  minutes; a `DecompileAddrs.java` post-script (addresses as script args) decompiles in one go.
- A full `objdump -d` of `.text` (6.1 M lines) is the fastest way to find message constructors and
  their callers (`grep` the vtable immediate or `call <ctor>`).
- x86 encodes small displacements in one byte: `fld dword [ecx+0x6C]` is `D9 41 6C`, not
  `D9 81 6C 00 00 00`. Write patterns from the raw bytes, never from the mnemonic.
