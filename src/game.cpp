#include "game.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "config.h"
#include "log.h"
#include "mem.h"

namespace f3cc::game {
namespace {

// The engine registers Lua-visible methods through Despair::Reflection method
// proxies, keyed on the method-name string. Two code shapes:
//   Pattern A: B8 <impl> 89 47 20 33 C0 C7 47 10 <name> 89 5F 14 89 77 18 C7 07 <proxyVtbl>
//              (registering function's prologue: 83 EC ?? 53 55 56 33 F6 57 BB <static>)
//   Pattern B: B8 <fn> 50 68 <name> E8 rel32 83 C4 0C 50 B9 <static> E8
// The statics are class descriptors, not instances, so the wrappers cannot be
// called through them; the mod reads them for the vtable slots, component type
// ids and helper functions they encode. HasPlayerStartedLevel is the one
// wrapper that is called: it uses no `this`.
using LevelFn = bool(__stdcall*)(int player);
using GetPlayerFn = void*(__cdecl*)(int player);
using ThisCall0 = void*(__fastcall*)(void* self, void* edx);
using ThisCall0B = bool(__fastcall*)(void* self, void* edx);
using ThisCall0I = int(__fastcall*)(void* self, void* edx);
using ThisCall0F = float(__fastcall*)(void* self, void* edx);
using ThisCall1I = void*(__fastcall*)(void* self, void* edx, int arg);
using ThisCall1P = void*(__fastcall*)(void* self, void* edx, const void* arg);
using ThisCallSetI = void(__fastcall*)(void* self, void* edx, int arg);
using ThisCall2I = void(__fastcall*)(void* self, void* edx, int a, int b);
using GetComponentsFn = void(__fastcall*)(void* self, void* edx, const void* type, void* vec);
using HeapFn = void*(__cdecl*)();
using FreeFn = void(__cdecl*)(void* p, unsigned n, void* heap);
using MsgCtorFn = void*(__fastcall*)(void* self, void* edx, int on, int allow_damage);
using SendMsgFn = void(__fastcall*)(void* object, void* edx, void* message);

uintptr_t g_base = 0;
mem::Range g_text;
std::vector<mem::Range> g_data;

uintptr_t g_game_static = 0;   // GameScriptGame class descriptor
uintptr_t g_fear_static = 0;   // GameScriptFEAR class descriptor
uintptr_t g_pause_impl = 0;
LevelFn g_level = nullptr;
GetPlayerFn g_get_player = nullptr;
uintptr_t g_get_player_object = 0;  // the game's "player index -> controlled object" helper (read, not called)

uint32_t g_off_menumgr = 0;
int g_slot_pause = -1;

// IPlayerComponent / IActorComponent slots, from the game's own code.
int g_slot_player_actor = 0;        // IPlayerComponent: the actor component it controls
int g_slot_player_avatar = 1;       // IPlayerComponent: its avatar object
int g_slot_actor_object = -1;       // IActorComponent: the game object
int g_slot_actor_indestructible = -1;  // IActorComponent: SetIndestructible(on, allowDamage)

const void* g_type[kTypeCount] = {};
char g_type_name[kTypeCount][48] = {};
int g_slot_get_components = -1;     // container at object+0x14

// Inventory lists (from GetInventoryItems).
int g_inv_slot_count = -1, g_inv_slot_list = -1, g_inv_slot_items = -1, g_inv_slot_item = -1;
uint32_t g_inv_item_handle_off = 0;
HeapFn g_heap = nullptr;
FreeFn g_free = nullptr;

// IndestructibleMessage: the game's constructor and the object's send routine.
MsgCtorFn g_msg_ctor = nullptr;
SendMsgFn g_send = nullptr;
uintptr_t g_indestructible_vt = 0;

// ISlowMoComponent (from RefillSlowMo / GetSlowMoPercentRemaining).
int g_slot_slowmo_max = -1, g_slot_slowmo_set = -1, g_slot_slowmo_cur = -1;

// Possession (from GameScriptPlayer's ResetPossessionTimer / GetMaxPossessionTime).
int g_slot_poss_state = 1;        // IPossessionComponent: state (the value below = possessing)
int g_slot_poss_victim = -1;      // IPossessionComponent: the body's IPossessedComponent
int g_slot_poss_reset = -1;       // IPossessionComponent: ResetTimer
int g_slot_victim_max = -1;       // IPossessedComponent: max possession time
constexpr int kSlotVictimActivate = 3, kSlotVictimStartedAt = 5, kSlotVictimPossessor = 7, kSlotVictimReset = 18;
uintptr_t g_clock = 0;            // the engine's double game clock, from the body's activate accessor

// IAmmoComponent slots. These are fixed by the interface; every one is
// verified against the accessor bytes before the interface is used.
constexpr int kAmmoSlotSetGun = 10, kAmmoSlotGun = 11, kAmmoSlotCarried = 13, kAmmoSlotMaxGun = 14,
              kAmmoSlotMaxCarried = 7, kAmmoSlotSetCarried = 39, kAmmoSlotInfinite = 30;

struct ReceiverInfo {
  const char* mangled;
  const char* label;
  uintptr_t vtable = 0;   // the Implementation<C, M> vtable (RTTI)
  uintptr_t guard = 0;    // the lazy-init guard the game sets when it first registers
  uintptr_t global = 0;   // the global receiver object whose first dword is the vtable pointer
  uintptr_t my_vtable[4] = {};
  DispatchFn original = nullptr;
  bool hooked = false;
  bool warned = false;
};
ReceiverInfo g_recv[kReceiverCount] = {
    {".?AV?$Implementation@VAmmoComponent@Despair@@VWeaponFireMessage@2@@ReflectionReceiver@Despair@@",
     "AmmoComponent<-WeaponFireMessage"},
    {".?AV?$Implementation@VBasicHitPointComponent@Despair@@VDamageMessage@2@@ReflectionReceiver@Despair@@",
     "BasicHitPointComponent<-DamageMessage"},
    {".?AV?$Implementation@VHitPointControllerComponent@Despair@@VDamageMessage@2@@ReflectionReceiver@Despair@@",
     "HitPointControllerComponent<-DamageMessage"},
    {".?AV?$Implementation@VLocationalHitPointControllerComponent@Despair@@VDamageMessage@2@@ReflectionReceiver@Despair@@",
     "LocationalHitPointControllerComponent<-DamageMessage"},
};

// The live menu manager, found by vtable scan on the mod thread.
std::vector<mem::VtableInfo> g_mm_vts;
uintptr_t g_mm_vt0 = 0, g_mm_sub_off = 0, g_mm_sub_vt = 0;
volatile uintptr_t g_mm_obj = 0;

bool g_ready = false;
char g_status[160] = "not started";

constexpr const char* kRttiMenuMgr = ".?AVMenuMgr@Despair@@";
constexpr const char* kRttiIndestructibleMessage = ".?AVIndestructibleMessage@Despair@@";

void set_status(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(g_status, sizeof(g_status), fmt, args);
  va_end(args);
}

uintptr_t rva(uintptr_t a) { return a ? a - g_base : 0; }

mem::Pattern with_imm32(const char* head, uint32_t imm, const char* tail = "") {
  mem::Pattern p = mem::parse_pattern(head);
  for (int i = 0; i < 4; ++i) p.bytes.push_back(static_cast<int16_t>((imm >> (8 * i)) & 0xFF));
  mem::Pattern t = mem::parse_pattern(tail);
  p.bytes.insert(p.bytes.end(), t.bytes.begin(), t.bytes.end());
  return p;
}

// The target of an E8 rel32 at `site`, or 0 when it leaves .text.
uintptr_t call_target(uintptr_t site) {
  if (mem::read<uint8_t>(site) != 0xE8) return 0;
  const uintptr_t t = site + 5 + static_cast<uintptr_t>(mem::read<int32_t>(site + 1));
  return g_text.contains(t) ? t : 0;
}

bool find_a(const char* name, uintptr_t* impl, uintptr_t* site) {
  const uintptr_t s = mem::find_cstring(g_data, name);
  if (!s) { logf("ERROR: name string '%s' not found in the exe", name); return false; }
  const mem::Pattern p = with_imm32("B8 ?? ?? ?? ?? 89 47 20 33 C0 C7 47 10", static_cast<uint32_t>(s));
  const uintptr_t a = mem::find_pattern(g_text, p);
  if (!a) { logf("ERROR: registration of '%s' not found", name); return false; }
  const uintptr_t fn = mem::read<uint32_t>(a + 1);
  if (!g_text.contains(fn)) { logf("ERROR: '%s' impl outside .text", name); return false; }
  *impl = fn;
  *site = a;
  logf("found %-30s impl=exe+0x%06X", name, rva(fn));
  return true;
}

bool find_owner_a(uintptr_t site, uintptr_t* out) {
  const mem::Pattern prologue = mem::parse_pattern("83 EC ?? 53 55 56 33 F6 57 BB");
  const mem::Range whole = mem::module_range(reinterpret_cast<HMODULE>(g_base));
  const uintptr_t low = site > g_text.begin + 0x4000 ? site - 0x4000 : g_text.begin;
  for (uintptr_t a = site; a >= low; --a) {
    const mem::Range r{a, a + prologue.bytes.size()};
    if (mem::find_pattern(r, prologue) != a) continue;
    const uintptr_t obj = mem::read<uint32_t>(a + 10);
    if (!whole.contains(obj) || g_text.contains(obj)) continue;
    *out = obj;
    return true;
  }
  return false;
}

bool find_b(const char* name, uintptr_t* fn, uintptr_t* owner) {
  const uintptr_t s = mem::find_cstring(g_data, name);
  if (!s) { logf("ERROR: name string '%s' not found in the exe", name); return false; }
  const mem::Pattern p = with_imm32("B8 ?? ?? ?? ?? 50 68", static_cast<uint32_t>(s),
                                    "E8 ?? ?? ?? ?? 83 C4 0C 50 B9 ?? ?? ?? ?? E8");
  const uintptr_t a = mem::find_pattern(g_text, p);
  if (!a) { logf("ERROR: registration of '%s' not found", name); return false; }
  const uintptr_t f = mem::read<uint32_t>(a + 1);
  const uintptr_t o = mem::read<uint32_t>(a + 21);
  const mem::Range whole = mem::module_range(reinterpret_cast<HMODULE>(g_base));
  if (!g_text.contains(f) || !whole.contains(o) || g_text.contains(o)) {
    logf("ERROR: '%s' fn/owner look wrong", name);
    return false;
  }
  *fn = f;
  *owner = o;
  logf("found %-30s fn=exe+0x%06X", name, rva(f));
  return true;
}

// Pattern D (GameScriptPlayer): B8 <impl> 50 68 <name> E8 rel32 83 C4 0C 50 8B CD E8
bool find_d(const char* name, uintptr_t* impl) {
  const uintptr_t s = mem::find_cstring(g_data, name);
  if (!s) { logf("ERROR: name string '%s' not found in the exe", name); return false; }
  const mem::Pattern p = with_imm32("B8 ?? ?? ?? ?? 50 68", static_cast<uint32_t>(s), "E8 ?? ?? ?? ?? 83 C4 0C 50 8B CD E8");
  const uintptr_t a = mem::find_pattern(g_text, p);
  if (!a) { logf("ERROR: registration of '%s' not found", name); return false; }
  const uintptr_t f = mem::read<uint32_t>(a + 1);
  if (!g_text.contains(f)) { logf("ERROR: '%s' impl outside .text", name); return false; }
  *impl = f;
  logf("found %-30s impl=exe+0x%06X", name, rva(f));
  return true;
}

// A component type id from its registration: push "IFooComponent"; mov ecx, id; call Register.
bool type_by_name(const char* name, Type t) {
  const uintptr_t s = mem::find_cstring(g_data, name);
  if (!s) { logf("ERROR: type name '%s' not found in the exe", name); return false; }
  const uintptr_t a = mem::find_pattern(g_text, with_imm32("68", static_cast<uint32_t>(s), "B9 ?? ?? ?? ?? E8"));
  if (!a) { logf("ERROR: registration of type '%s' not found", name); return false; }
  const uintptr_t id = mem::read<uint32_t>(a + 6);
  const mem::Range whole = mem::module_range(reinterpret_cast<HMODULE>(g_base));
  if (!whole.contains(id) || g_text.contains(id)) { logf("ERROR: type '%s' id %p is not a static", name, reinterpret_cast<void*>(id)); return false; }
  g_type[t] = reinterpret_cast<const void*>(id);
  std::snprintf(g_type_name[t], sizeof(g_type_name[t]), "%.47s", name);
  logf("component type %s = exe+0x%X", name, rva(id));
  return true;
}

// A component type id (the static a GetComponent call pushes) at `pat`'s
// wildcard imm32 inside the wrapper at `fn`.
bool type_from(const char* wrapper, uintptr_t fn, size_t span, const char* pat, size_t imm_at, Type t) {
  const uintptr_t a = mem::find_pattern(mem::Range{fn, fn + span}, pat);
  if (!a) { logf("ERROR: %s does not have the expected GetComponent shape", wrapper); return false; }
  const uintptr_t id = mem::read<uint32_t>(a + imm_at);
  const mem::Range whole = mem::module_range(reinterpret_cast<HMODULE>(g_base));
  if (!whole.contains(id) || g_text.contains(id)) { logf("ERROR: %s: type id %p is not a static", wrapper, reinterpret_cast<void*>(id)); return false; }
  g_type[t] = reinterpret_cast<const void*>(id);
  // Its registered name: `push "IFooComponent"; mov ecx, id; call Register`.
  const uintptr_t r = mem::find_pattern(g_text, with_imm32("68 ?? ?? ?? ?? B9", static_cast<uint32_t>(id), "E8"));
  if (r) {
    const uintptr_t s = mem::read<uint32_t>(r + 1);
    if (mem::readable(reinterpret_cast<void*>(s), 48)) std::snprintf(g_type_name[t], sizeof(g_type_name[t]), "%.47s", reinterpret_cast<const char*>(s));
  }
  if (!g_type_name[t][0]) std::snprintf(g_type_name[t], sizeof(g_type_name[t]), "type@exe+0x%X", rva(id));
  logf("%s -> component type %s (exe+0x%X)", wrapper, g_type_name[t], rva(id));
  return true;
}

bool parse_pause() {
  const mem::Range pauser{g_pause_impl, g_pause_impl + 0x20};
  if (uintptr_t a = mem::find_pattern(pauser, "8B 49 08 E8 ?? ?? ?? ?? 8B 10 8B C8 8B 82 ?? ?? ?? ?? FF E0")) {
    g_slot_pause = static_cast<int>(mem::read<uint32_t>(a + 14) / 4);
    const uintptr_t getter = a + 8 + mem::read<int32_t>(a + 4);
    const mem::Range gr{getter, getter + 8};
    if (g_text.contains(getter) && mem::find_pattern(gr, "8B 81 ?? ?? ?? ?? C3") == getter)
      g_off_menumgr = mem::read<uint32_t>(getter + 2);
  }
  if (g_slot_pause < 0) { logf("ERROR: could not parse IsPauseMenuShowing"); return false; }
  logf("IsPauseMenuShowing: MenuMgr at ctx+0x%X, slot %d", g_off_menumgr, g_slot_pause);
  return true;
}

// HasPlayerStartedLevel: mov eax,[esp+4]; push eax; call GetPlayer; ...; player->vtbl[N]()
bool parse_level(uintptr_t fn) {
  const mem::Range r{fn, fn + 0x30};
  if (mem::find_pattern(r, "8B 44 24 04 50 E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 ?? 8B 10 8B C8 8B 82 ?? ?? ?? ?? FF D0 C2 04 00") != fn) {
    logf("ERROR: HasPlayerStartedLevel does not have the expected shape");
    return false;
  }
  const uintptr_t gp = call_target(fn + 5);
  if (!gp) { logf("ERROR: GetPlayer call not found"); return false; }
  g_get_player = reinterpret_cast<GetPlayerFn>(gp);
  logf("GetPlayer=exe+0x%06X (HasPlayerStartedLevel uses IPlayerComponent slot %u)", rva(gp), mem::read<uint32_t>(fn + 0x17) / 4);
  return true;
}

// The game's own "player index -> the object its character is": GetPlayer,
// then IPlayerComponent slot 0 (the actor component), then IActorComponent
// slot 12 (the object). Located by its shape around a call to GetPlayer.
bool parse_player_object() {
  const mem::Pattern p = mem::parse_pattern(
      "8B 44 24 04 50 E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 ?? 8B 10 8B C8 8B 02 FF D0 85 C0 74 ?? 8B 10 8B C8 8B 42 ?? FF E0");
  for (uintptr_t a : mem::find_all(g_text, p, 32)) {
    if (call_target(a + 5) != reinterpret_cast<uintptr_t>(g_get_player)) continue;
    g_get_player_object = a;
    g_slot_player_actor = 0;
    g_slot_actor_object = mem::read<uint8_t>(a + 0x23) / 4;
    logf("GetPlayerObject=exe+0x%06X: IPlayerComponent slot %d -> IActorComponent slot %d -> object",
         rva(a), g_slot_player_actor, g_slot_actor_object);
    return true;
  }
  logf("ERROR: the player-object helper was not found around GetPlayer");
  return false;
}

// The game's "set player indestructible": object = GetPlayerObject(idx);
// comp = GetComponent(object, IActorComponent); if (comp) comp->vtbl[16](on, 0);
// else { IndestructibleMessage msg(on, 0); object->Send(&msg); }
bool parse_indestructible() {
  const mem::Pattern p = mem::parse_pattern(
      "8B F0 83 C4 04 85 F6 74 ?? 8B 56 04 8B 02 8D 4E 04 68 ?? ?? ?? ?? FF D0 6A 00 53 85 C0 74 ?? 8B 10 8B C8 8B 42 ?? "
      "FF D0 5E 5B 83 C4 08 C2 04 00 8D 4C 24 ?? E8 ?? ?? ?? ?? 50 8B CE E8");
  const mem::Range whole = mem::module_range(reinterpret_cast<HMODULE>(g_base));
  for (uintptr_t a : mem::find_all(g_text, p, 16)) {
    if (call_target(a - 5) != g_get_player_object) continue;
    const uintptr_t id = mem::read<uint32_t>(a + 0x12);
    const uintptr_t ctor = call_target(a + 0x34);
    const uintptr_t send = call_target(a + 0x3C);
    if (!whole.contains(id) || g_text.contains(id) || !ctor || !send) continue;
    // The constructor must build exactly {vtable, bool, bool} with the RTTI vtable.
    std::vector<mem::VtableInfo> vts = mem::class_vtables(reinterpret_cast<HMODULE>(g_base), kRttiIndestructibleMessage);
    for (const mem::VtableInfo& v : vts) if (v.offset == 0) g_indestructible_vt = v.vtable;
    if (!g_indestructible_vt) { logf("ERROR: IndestructibleMessage vtable not found"); return false; }
    const mem::Pattern cp = with_imm32("8A 54 24 08 8B C1 8A 4C 24 04 C7 00", static_cast<uint32_t>(g_indestructible_vt), "88 48 04 88 50 05 C2 08 00");
    if (mem::find_pattern(mem::Range{ctor, ctor + 0x20}, cp) != ctor) { logf("ERROR: IndestructibleMessage constructor has an unexpected shape"); return false; }
    // Send: `add ecx, <router offset>; jmp Router::Send`.
    if (mem::find_pattern(mem::Range{send, send + 8}, "83 C1 ?? E9") != send) { logf("ERROR: object Send has an unexpected shape"); return false; }
    g_type[kActor] = reinterpret_cast<const void*>(id);
    g_slot_actor_indestructible = mem::read<uint8_t>(a + 0x25) / 4;
    g_msg_ctor = reinterpret_cast<MsgCtorFn>(ctor);
    g_send = reinterpret_cast<SendMsgFn>(send);
    const uintptr_t r = mem::find_pattern(g_text, with_imm32("68 ?? ?? ?? ?? B9", static_cast<uint32_t>(id), "E8"));
    if (r && mem::readable(reinterpret_cast<void*>(mem::read<uint32_t>(r + 1)), 48))
      std::snprintf(g_type_name[kActor], sizeof(g_type_name[kActor]), "%.47s", reinterpret_cast<const char*>(mem::read<uint32_t>(r + 1)));
    else
      std::snprintf(g_type_name[kActor], sizeof(g_type_name[kActor]), "type@exe+0x%X", rva(id));
    logf("SetPlayerIndestructible=exe+0x%06X: %s slot %d, IndestructibleMessage ctor exe+0x%06X (vtable exe+0x%X), "
         "object Send exe+0x%06X (router at +0x%X)",
         rva(a - 5), g_type_name[kActor], g_slot_actor_indestructible, rva(ctor), rva(g_indestructible_vt), rva(send),
         mem::read<uint8_t>(send + 2));
    return true;
  }
  logf("ERROR: the player-indestructible helper was not found");
  return false;
}

bool parse_inventory(uintptr_t fn) {
  const mem::Range r{fn, fn + 0x240};
  const uintptr_t a = mem::find_pattern(r, "8D 48 14 8B 01 8B 40 08 57 8D 54 24 ?? 52 68 ?? ?? ?? ?? FF D0");
  if (!a) { logf("ERROR: GetInventoryItems: no GetComponents call"); return false; }
  g_slot_get_components = mem::read<uint8_t>(a + 7) / 4;
  if (!type_from("GetInventoryItems", a, 0x20, "68 ?? ?? ?? ?? FF D0", 1, kInventoryList)) return false;
  const uintptr_t c = mem::find_pattern(r, "8B 82 ?? ?? ?? ?? FF D0 3B C3 89 44 24");
  const uintptr_t l = mem::find_pattern(r, "8B 92 ?? ?? ?? ?? 50 FF D2 8B F8");
  const uintptr_t n = mem::find_pattern(r, "8B 90 ?? ?? ?? ?? 8B CF FF D2 8B D8");
  const uintptr_t i = mem::find_pattern(r, "8B 90 ?? ?? ?? ?? 55 8B CF FF D2 85 C0 74 ?? 8D 48 ?? E8");
  const uintptr_t f = mem::find_pattern(r, "E8 ?? ?? ?? ?? 50 56 57 E8 ?? ?? ?? ?? 83 C4 0C");
  if (!c || !l || !n || !i || !f) { logf("ERROR: GetInventoryItems: item walk shape not matched (%d%d%d%d%d)", !!c, !!l, !!n, !!i, !!f); return false; }
  g_inv_slot_count = static_cast<int>(mem::read<uint32_t>(c + 2) / 4);
  g_inv_slot_list = static_cast<int>(mem::read<uint32_t>(l + 2) / 4);
  g_inv_slot_items = static_cast<int>(mem::read<uint32_t>(n + 2) / 4);
  g_inv_slot_item = static_cast<int>(mem::read<uint32_t>(i + 2) / 4);
  g_inv_item_handle_off = mem::read<uint8_t>(i + 0x11);
  const uintptr_t heap = call_target(f), fr = call_target(f + 8);
  if (!heap || !fr) { logf("ERROR: GetInventoryItems: allocator calls not found"); return false; }
  g_heap = reinterpret_cast<HeapFn>(heap);
  g_free = reinterpret_cast<FreeFn>(fr);
  logf("inventory: GetComponents slot %d; list slots count=%d get=%d, group slots items=%d get=%d, item handle at +0x%X; "
       "heap=exe+0x%06X free=exe+0x%06X",
       g_slot_get_components, g_inv_slot_count, g_inv_slot_list, g_inv_slot_items, g_inv_slot_item, g_inv_item_handle_off,
       rva(heap), rva(fr));
  return true;
}

bool parse_slowmo(uintptr_t refill, uintptr_t percent) {
  if (!type_from("RefillSlowMo", refill, 0x70,
                 "8B 01 8B 10 68 ?? ?? ?? ?? FF D2 8B F0 85 F6 74 ?? 8B 06 8B 50 ?? 8B CE FF D2 D9 5C 24 ?? 8B 06 D9 44 24 ?? 8B 50 ??",
                 5, kSlowMo))
    return false;
  const uintptr_t a = mem::find_pattern(mem::Range{refill, refill + 0x70}, "8B 06 8B 50 ?? 8B CE FF D2 D9 5C 24 ?? 8B 06 D9 44 24 ?? 8B 50 ??");
  g_slot_slowmo_max = mem::read<uint8_t>(a + 4) / 4;
  g_slot_slowmo_set = mem::read<uint8_t>(a + 0x15) / 4;
  const uintptr_t b = mem::find_pattern(mem::Range{percent, percent + 0x70}, "8B 06 8B 50 ?? 8B CE FF D2 D8 74 24");
  if (!b) { logf("ERROR: GetSlowMoPercentRemaining has an unexpected shape"); return false; }
  g_slot_slowmo_cur = mem::read<uint8_t>(b + 4) / 4;
  logf("ISlowMoComponent slots: max=%d set=%d current=%d", g_slot_slowmo_max, g_slot_slowmo_set, g_slot_slowmo_cur);
  return true;
}

// ResetPossessionTimer(idx): comp = host->PossessionComponentForPlayer(idx); comp->vtbl[N]()
// GetMaxPossessionTime(h): body = comp->vtbl[M](); body->vtbl[K]()
bool parse_possession(uintptr_t reset, uintptr_t maxtime) {
  const uintptr_t a = mem::find_pattern(mem::Range{reset, reset + 0xC0}, "8B 10 8B C8 8B 42 ?? FF D0");
  if (!a) { logf("ERROR: ResetPossessionTimer has an unexpected shape"); return false; }
  g_slot_poss_reset = mem::read<uint8_t>(a + 6) / 4;
  const mem::Range mr{maxtime, maxtime + 0x20};
  const uintptr_t b = mem::find_pattern(mr, "E8 ?? ?? ?? ?? 8B 10 8B C8 8B 42 ?? FF D0");
  if (!b) { logf("ERROR: GetMaxPossessionTime has an unexpected shape"); return false; }
  g_slot_victim_max = mem::read<uint8_t>(b + 11) / 4;
  const uintptr_t helper = call_target(b);
  const uintptr_t c = helper ? mem::find_pattern(mem::Range{helper, helper + 0x40}, "8B 10 8B C8 8B 42 ?? FF D0 85 C0 75") : 0;
  if (!c) { logf("ERROR: GetMaxPossessionTime's body lookup has an unexpected shape"); return false; }
  g_slot_poss_victim = mem::read<uint8_t>(c + 6) / 4;
  logf("possession slots: IPossessionComponent state=%d body=%d reset=%d; IPossessedComponent max=%d", g_slot_poss_state,
       g_slot_poss_victim, g_slot_poss_reset, g_slot_victim_max);
  return true;
}

bool resolve_receivers() {
  HMODULE exe = reinterpret_cast<HMODULE>(g_base);
  bool all = true;
  for (ReceiverInfo& r : g_recv) {
    for (const mem::VtableInfo& v : mem::class_vtables(exe, r.mangled)) if (v.offset == 0) r.vtable = v.vtable;
    if (!r.vtable) { logf("ERROR: receiver vtable for %s not found", r.label); all = false; continue; }
    // The lazy initialiser: or [guard], eax ; mov [global], vtable
    const uintptr_t a = mem::find_pattern(g_text, with_imm32("09 05 ?? ?? ?? ?? C7 05 ?? ?? ?? ??", static_cast<uint32_t>(r.vtable)));
    if (!a) { logf("ERROR: receiver initialiser for %s not found", r.label); all = false; continue; }
    r.guard = mem::read<uint32_t>(a + 2);
    r.global = mem::read<uint32_t>(a + 8);
    logf("receiver %s: vtable exe+0x%X, global exe+0x%X, guard exe+0x%X", r.label, rva(r.vtable), rva(r.global), rva(r.guard));
  }
  return all;
}

bool resolve_class_vtables() {
  HMODULE exe = reinterpret_cast<HMODULE>(g_base);
  g_mm_vts = mem::class_vtables(exe, kRttiMenuMgr);
  for (const mem::VtableInfo& v : g_mm_vts) {
    if (v.offset == 0) g_mm_vt0 = v.vtable;
    uintptr_t fn = 0;
    if (mem::read_safe(v.vtable + static_cast<uintptr_t>(g_slot_pause) * 4, &fn) && g_text.contains(fn)) {
      const mem::Range r{fn, fn + 8};
      if (mem::find_pattern(r, "80 79 ?? 00 74") == fn) { g_mm_sub_off = v.offset; g_mm_sub_vt = v.vtable; }
    }
  }
  if (!g_mm_vt0 || !g_mm_sub_vt) { logf("ERROR: MenuMgr vtables not resolved"); return false; }
  logf("MenuMgr vtable exe+0x%X, IsPauseMenuShowing on subobject +0x%X", rva(g_mm_vt0), g_mm_sub_off);
  return true;
}

bool scan() {
  HMODULE exe = GetModuleHandleA(nullptr);
  g_base = reinterpret_cast<uintptr_t>(exe);
  g_text = mem::section(exe, ".text");
  g_data = mem::data_sections(exe);
  if (g_text.empty() || g_data.empty()) { set_status("exe sections not found"); return false; }
  logf("exe base %p, .text exe+0x%X..0x%X", reinterpret_cast<void*>(g_base), rva(g_text.begin), rva(g_text.end));

  uintptr_t level = 0, o1 = 0, o2 = 0, o3 = 0, o4 = 0, o5 = 0, o6 = 0;
  uintptr_t ammo_pct = 0, hp_prio = 0, hp_pct = 0, inv = 0;
  if (!find_b("IsPauseMenuShowing", &g_pause_impl, &o1)) return false;
  if (!find_b("HasPlayerStartedLevel", &level, &o2)) return false;
  if (!find_b("SetCurrentTotalAmmoPercent", &ammo_pct, &o3)) return false;
  if (!find_b("SetHitPointPriorityInvulnerable", &hp_prio, &o4)) return false;
  if (!find_b("SetHitPointPercent", &hp_pct, &o5)) return false;
  if (!find_b("GetInventoryItems", &inv, &o6)) return false;
  if (o1 != o2 || o1 != o3 || o1 != o4 || o1 != o5 || o1 != o6) { logf("ERROR: the Game wrappers do not share one class descriptor"); return false; }
  g_game_static = o1;
  uintptr_t refill = 0, percent = 0, s1 = 0, s2 = 0, f1 = 0, f2 = 0;
  if (!find_a("RefillSlowMo", &refill, &s1) || !find_a("GetSlowMoPercentRemaining", &percent, &s2)) return false;
  if (!find_owner_a(s1, &f1) || !find_owner_a(s2, &f2) || f1 != f2) { logf("ERROR: the FEAR wrappers do not share one class descriptor"); return false; }
  g_fear_static = f1;
  g_level = reinterpret_cast<LevelFn>(level);
  logf("class descriptors: GameScriptGame@exe+0x%X GameScriptFEAR@exe+0x%X", rva(g_game_static), rva(g_fear_static));

  if (!parse_pause() || !parse_level(level) || !parse_player_object() || !parse_indestructible()) return false;
  if (!type_from("SetCurrentTotalAmmoPercent", ammo_pct, 0xA0,
                 "8B 01 8B 10 68 ?? ?? ?? ?? FF D2 85 C0 74 ?? 8B 10 D9 44 24 ?? 51 8B C8 D9 1C 24 8B 42 ?? FF D0", 5, kAmmo))
    return false;
  if (!type_from("SetHitPointPriorityInvulnerable", hp_prio, 0xA0, "8B 01 8B 10 68 ?? ?? ?? ?? FF D2 8B F0 85 F6 74", 5, kHitPointController))
    return false;
  if (!type_from("SetHitPointPercent", hp_pct, 0x100, "8B 02 68 ?? ?? ?? ?? FF D0 85 C0 74 ?? 8B 10 D9 44 24", 3, kHitPoint))
    return false;
  if (!parse_inventory(inv) || !parse_slowmo(refill, percent)) return false;
  uintptr_t poss_reset = 0, poss_max = 0;
  if (find_d("ResetPossessionTimer", &poss_reset) && find_d("GetMaxPossessionTime", &poss_max) &&
      type_by_name("IPossessionComponent", kPossession) && type_by_name("IPossessedComponent", kPossessed)) {
    if (!parse_possession(poss_reset, poss_max)) g_slot_poss_reset = -1;
  }
  if (g_slot_poss_reset < 0) logf("possession cheat unavailable (see above)");
  if (!resolve_class_vtables()) return false;
  resolve_receivers();  // a missing receiver only disables the hook that needs it
  return true;
}

bool identity_ok(uintptr_t obj, const char* expected, bool* pending) {
  uintptr_t p = 0;
  *pending = false;
  if (!mem::read_safe(obj + 0xC, &p)) return false;
  if (!p) { *pending = true; return false; }
  const size_t n = std::strlen(expected);
  if (!mem::readable(reinterpret_cast<void*>(p), n + 1)) return false;
  return std::memcmp(reinterpret_cast<const void*>(p), expected, n) == 0;  // prefix: "GameScriptFEAR..." 
}

// The class descriptors carry the class name at +0xC once constructed; that is
// the check that the objects the signature scan named are what we think.
bool verify_identity() {
  const DWORD start = GetTickCount();
  for (;;) {
    bool pa = false, pb = false;
    const bool a = identity_ok(g_game_static, "GameScriptGame", &pa);
    const bool b = identity_ok(g_fear_static, "GameScriptFEAR", &pb);
    if (a && b) { logf("class descriptors identified by name"); return true; }
    if (!pa && !pb) { set_status("script classes are not what the scan expected"); logf("ERROR: class descriptor names do not match (Game ok=%d, FEAR ok=%d)", a, b); return false; }
    if (GetTickCount() - start > 60000) { set_status("script classes never constructed"); return false; }
    Sleep(50);
  }
}

bool is_instance(uintptr_t obj, const std::vector<mem::VtableInfo>& vts) {
  for (const mem::VtableInfo& v : vts) {
    uintptr_t vp = 0;
    if (!mem::read_safe(obj + v.offset, &vp) || vp != v.vtable) return false;
  }
  return true;
}

uintptr_t scan_for(const char* what, uintptr_t vt0, const std::vector<mem::VtableInfo>& vts) {
  const DWORD t0 = GetTickCount();
  size_t scanned = 0;
  std::vector<uintptr_t> hits = mem::find_objects_by_vtable(vt0, 16, &scanned);
  for (uintptr_t h : hits)
    if (is_instance(h, vts)) {
      logf("%s instance %p (%u candidate(s), %u MB scanned in %lu ms)", what, reinterpret_cast<void*>(h),
           static_cast<unsigned>(hits.size()), static_cast<unsigned>(scanned >> 20), GetTickCount() - t0);
      return h;
    }
  return 0;
}

uintptr_t menu_mgr_obj() {
  const uintptr_t o = g_mm_obj;
  if (!o) return 0;
  uintptr_t vp = 0;
  if (!mem::read_safe(o, &vp) || vp != g_mm_vt0) { g_mm_obj = 0; logf("menu manager %p is gone - rescanning", reinterpret_cast<void*>(o)); return 0; }
  return o;
}

// A virtual function of `self`, if the object and its vtable slot are readable and in .text.
template <typename Fn>
Fn vfn(void* self, int slot) {
  uintptr_t vt = 0, fn = 0;
  if (slot < 0 || !mem::read_safe(reinterpret_cast<uintptr_t>(self), &vt) || !vt) return nullptr;
  if (!mem::read_safe(vt + static_cast<uintptr_t>(slot) * 4, &fn) || !g_text.contains(fn)) return nullptr;
  return reinterpret_cast<Fn>(fn);
}

bool slot_matches(void* self, int slot, const char* pattern) {
  auto fn = reinterpret_cast<uintptr_t>(vfn<void*>(self, slot));
  if (!fn) return false;
  const mem::Pattern p = mem::parse_pattern(pattern);
  return mem::find_pattern(mem::Range{fn, fn + p.bytes.size()}, p) == fn;
}

// --- per-vtable layout verification --------------------------------------------
// Both interfaces are implemented by one class each in practice, but the check
// is keyed on the vtable so a subclass gets its own verification.
struct AmmoVt { uintptr_t vtable; int iface_off; };
AmmoVt g_ammo_ok[4] = {};
int g_ammo_ok_n = 0;
uintptr_t g_ammo_bad = 0;

struct SlowMoVt { uintptr_t vtable; uint32_t off_max, off_cur; };
SlowMoVt g_slowmo_ok[4] = {};
int g_slowmo_ok_n = 0;
uintptr_t g_slowmo_bad = 0;

int col_offset(void* iface) {
  uintptr_t vt = 0, col = 0, off = 0, sig = 0;
  if (!mem::read_safe(reinterpret_cast<uintptr_t>(iface), &vt) || !vt) return -1;
  if (!mem::read_safe(vt - 4, &col) || !col || !mem::read_safe(col, &sig) || sig != 0) return -1;
  if (!mem::read_safe(col + 4, &off) || off > 0x10000) return -1;
  return static_cast<int>(off);
}

const AmmoVt* ammo_verified(void* ammo) {
  uintptr_t vt = 0;
  if (!mem::read_safe(reinterpret_cast<uintptr_t>(ammo), &vt) || !vt) return nullptr;
  for (int i = 0; i < g_ammo_ok_n; ++i) if (g_ammo_ok[i].vtable == vt) return &g_ammo_ok[i];
  if (vt == g_ammo_bad || g_ammo_ok_n >= 4) return nullptr;
  const int off = col_offset(ammo);
  struct Check { int slot; const char* bytes; const char* what; };
  static const Check kChecks[] = {
      {kAmmoSlotGun, "8B 81 ?? ?? ?? ?? C3", "ammo-in-gun getter"},
      {kAmmoSlotMaxGun, "8B 81 ?? ?? ?? ?? 0F AF 81 ?? ?? ?? ?? C3", "max-in-gun (clips * per clip)"},
      {kAmmoSlotMaxCarried, "8B 81 ?? ?? ?? ?? 0F AF 81 ?? ?? ?? ?? C3", "max-carried (clips * per clip)"},
      {kAmmoSlotCarried, "56 8B F1 8B 86 ?? ?? ?? ?? 85 C0 74", "carried getter (pool aware)"},
      {kAmmoSlotSetGun, "8B 44 24 04 56 8B F1 57 8D 7E ?? 50 8B CF E8", "set-in-gun"},
      {kAmmoSlotSetCarried, "8B 44 24 04 56 8D 71 ?? 50 8B CE E8", "set-carried"},
      {kAmmoSlotInfinite, "8B 81 ?? ?? ?? ?? D1 E8 83 E0 01 C3", "infinite-carried flag"},
  };
  bool ok = off > 0;
  for (const Check& c : kChecks)
    if (ok && !slot_matches(ammo, c.slot, c.bytes)) { logf("ERROR: IAmmoComponent slot %d is not the %s - not touching ammo", c.slot, c.what); ok = false; }
  if (ok) {
    // The setters adjust to the complete object with `lea esi,[ecx-off]`: it must be the RTTI offset.
    const auto set_gun = reinterpret_cast<uintptr_t>(vfn<void*>(ammo, kAmmoSlotSetGun));
    const auto set_carried = reinterpret_cast<uintptr_t>(vfn<void*>(ammo, kAmmoSlotSetCarried));
    const int d1 = -static_cast<int8_t>(mem::read<uint8_t>(set_gun + 10)), d2 = -static_cast<int8_t>(mem::read<uint8_t>(set_carried + 7));
    if (d1 != off || d2 != off) { logf("ERROR: IAmmoComponent setters adjust by %d/%d but RTTI says +0x%X", d1, d2, off); ok = false; }
  }
  if (!ok) { g_ammo_bad = vt; return nullptr; }
  g_ammo_ok[g_ammo_ok_n] = {vt, off};
  logf("IAmmoComponent layout verified (vtable exe+0x%X, interface at object+0x%X)", rva(vt), off);
  return &g_ammo_ok[g_ammo_ok_n++];
}

const SlowMoVt* slowmo_verified(void* sm) {
  uintptr_t vt = 0;
  if (!mem::read_safe(reinterpret_cast<uintptr_t>(sm), &vt) || !vt) return nullptr;
  for (int i = 0; i < g_slowmo_ok_n; ++i) if (g_slowmo_ok[i].vtable == vt) return &g_slowmo_ok[i];
  if (vt == g_slowmo_bad || g_slowmo_ok_n >= 4) return nullptr;
  // fld dword [ecx+max]; ret  /  fld dword [ecx+cur]; ret  /  the setter clamps to max and stores cur.
  const bool ok = slot_matches(sm, g_slot_slowmo_max, "D9 41 ?? C3") && slot_matches(sm, g_slot_slowmo_cur, "D9 41 ?? C3") &&
                  slot_matches(sm, g_slot_slowmo_set, "83 EC 08 D9 41 ?? D9 5C 24 04 D9 41 ?? D9 1C 24");
  if (!ok) { logf("ERROR: ISlowMoComponent accessors have an unexpected shape - not touching slow-mo"); g_slowmo_bad = vt; return nullptr; }
  const auto fmax = reinterpret_cast<uintptr_t>(vfn<void*>(sm, g_slot_slowmo_max));
  const auto fcur = reinterpret_cast<uintptr_t>(vfn<void*>(sm, g_slot_slowmo_cur));
  const auto fset = reinterpret_cast<uintptr_t>(vfn<void*>(sm, g_slot_slowmo_set));
  const uint32_t off_max = mem::read<uint8_t>(fmax + 2), off_cur = mem::read<uint8_t>(fcur + 2);
  if (mem::read<uint8_t>(fset + 5) != off_cur || mem::read<uint8_t>(fset + 12) != off_max) {
    logf("ERROR: ISlowMoComponent setter reads +0x%X/+0x%X, getters +0x%X/+0x%X - not touching slow-mo",
         mem::read<uint8_t>(fset + 5), mem::read<uint8_t>(fset + 12), off_cur, off_max);
    g_slowmo_bad = vt;
    return nullptr;
  }
  g_slowmo_ok[g_slowmo_ok_n] = {vt, off_max, off_cur};
  logf("ISlowMoComponent layout verified (vtable exe+0x%X): max at +0x%X, current at +0x%X", rva(vt),
       g_slowmo_ok[g_slowmo_ok_n].off_max, g_slowmo_ok[g_slowmo_ok_n].off_cur);
  return &g_slowmo_ok[g_slowmo_ok_n++];
}

}  // namespace

bool discover() {
  set_status("scanning");
  if (!scan()) { if (!std::strcmp(g_status, "scanning")) set_status("signature scan failed - see log"); return false; }
  if (!verify_identity()) return false;
  g_ready = true;
  set_status("ready");
  logf("game layer ready - looking for the menu manager");
  return true;
}

void instance_loop() {
  if (!g_ready) return;
  for (;;) {
    if (!g_mm_obj) g_mm_obj = scan_for("MenuMgr", g_mm_vt0, g_mm_vts);
    Sleep(g_mm_obj ? 1000 : 2000);
  }
}

bool ready() { return g_ready; }
bool context_ready() { return g_ready && g_mm_obj; }

bool pause_menu_showing() {
  const uintptr_t m = menu_mgr_obj();
  if (!m) return false;
  const uintptr_t sub = m + g_mm_sub_off;
  uintptr_t vp = 0;
  if (!mem::read_safe(sub, &vp) || vp != g_mm_sub_vt) return false;
  auto fn = mem::read<ThisCall0B>(g_mm_sub_vt + static_cast<uintptr_t>(g_slot_pause) * 4);
  return fn(reinterpret_cast<void*>(sub), nullptr);
}

bool level_started(int player) {
  if (!g_ready || player < 0 || player >= 16) return false;
  return g_level(player);
}

// --- players ------------------------------------------------------------------------
void* player(int index) {
  if (!g_ready || index < 0 || index >= 16) return nullptr;
  return g_get_player(index);
}

void* player_actor(void* p) {
  auto fn = vfn<ThisCall0>(p, g_slot_player_actor);
  return fn ? fn(p, nullptr) : nullptr;
}

void* player_avatar(void* p) {
  // mov eax,[ecx+X]; test; je; mov eax,[eax+8]; test; je; add eax,-0x18; ret  (handle -> object)
  if (!slot_matches(p, g_slot_player_avatar, "8B 41 ?? 85 C0 74 ?? 8B 40 08 85 C0 74 ?? 83 C0 E8 C3")) return nullptr;
  auto fn = vfn<ThisCall0>(p, g_slot_player_avatar);
  return fn ? fn(p, nullptr) : nullptr;
}

void* actor_object(void* actor) {
  auto fn = vfn<ThisCall0>(actor, g_slot_actor_object);
  return fn ? fn(actor, nullptr) : nullptr;
}

void set_indestructible(void* object, bool on, bool allow_damage) {
  if (!object) return;
  if (void* actor = get_component(object, kActor)) {
    if (auto fn = vfn<ThisCall2I>(actor, g_slot_actor_indestructible)) {
      fn(actor, nullptr, on ? 1 : 0, allow_damage ? 1 : 0);
      return;
    }
  }
  if (!g_msg_ctor || !g_send) return;
  uint8_t msg[16] = {};
  g_msg_ctor(msg, nullptr, on ? 1 : 0, allow_damage ? 1 : 0);
  g_send(object, nullptr, msg);
}

// --- components --------------------------------------------------------------------------
const char* type_name(Type t) { return t >= 0 && t < kTypeCount ? g_type_name[t] : "?"; }

void* get_component(void* object, Type t) {
  if (!object || t < 0 || t >= kTypeCount || !g_type[t]) return nullptr;
  void* container = static_cast<uint8_t*>(object) + 4;
  auto fn = vfn<ThisCall1P>(container, 0);
  return fn ? fn(container, nullptr, g_type[t]) : nullptr;
}

int get_components(void* object, Type t, void** out, int max) {
  if (!object || t < 0 || t >= kTypeCount || !g_type[t] || max <= 0 || g_slot_get_components < 0) return 0;
  void* container = static_cast<uint8_t*>(object) + 0x14;
  auto fn = vfn<GetComponentsFn>(container, g_slot_get_components);
  if (!fn) return 0;
  struct Vec { void** begin; void** end; void** cap; } v{nullptr, nullptr, nullptr};
  fn(container, nullptr, g_type[t], &v);
  int n = 0;
  if (v.begin && v.end >= v.begin) {
    for (void** p = v.begin; p < v.end && n < max; ++p) out[n++] = *p;
    g_free(v.begin, static_cast<unsigned>((v.cap - v.begin) * sizeof(void*)), g_heap());  // the wrapper frees the capacity
  }
  return n;
}

void* complete_object(void* iface) {
  const int off = col_offset(iface);
  return off < 0 ? nullptr : static_cast<uint8_t*>(iface) - off;
}

const char* rtti_of(void* obj) {
  const char* n = obj ? mem::rtti_name(obj) : nullptr;
  return n ? mem::rtti_short(n) : "?";
}

int inventory_objects(void* object, void** out, int max) {
  void* lists[16];
  const int nl = get_components(object, kInventoryList, lists, 16);
  int n = 0;
  for (int li = 0; li < nl && n < max; ++li) {
    void* list = lists[li];
    auto count = vfn<ThisCall0I>(list, g_inv_slot_count);
    auto get_group = vfn<ThisCall1I>(list, g_inv_slot_list);
    if (!count || !get_group) continue;
    const int groups = count(list, nullptr);
    for (int g = 0; g < groups && g < 64 && n < max; ++g) {
      void* group = get_group(list, nullptr, g);
      auto items = vfn<ThisCall0I>(group, g_inv_slot_items);
      auto get_item = vfn<ThisCall1I>(group, g_inv_slot_item);
      if (!items || !get_item) continue;
      const int m = items(group, nullptr);
      for (int j = 0; j < m && j < 64 && n < max; ++j) {
        void* item = get_item(group, nullptr, j);
        uintptr_t ctrl = 0, target = 0;
        if (!item || !mem::read_safe(reinterpret_cast<uintptr_t>(item) + g_inv_item_handle_off, &ctrl) || !ctrl) continue;
        if (!mem::read_safe(ctrl + 8, &target) || !target || target == 0x18) continue;
        out[n++] = reinterpret_cast<void*>(target - 0x18);
      }
    }
  }
  return n;
}

// --- ammo ---------------------------------------------------------------------------------
bool ammo_layout_ok(void* ammo) { return ammo_verified(ammo) != nullptr; }
int ammo_interface_offset() { return g_ammo_ok_n ? g_ammo_ok[0].iface_off : -1; }

namespace {
int ammo_call(void* ammo, int slot) {
  if (!ammo_verified(ammo)) return -1;
  auto fn = vfn<ThisCall0I>(ammo, slot);
  return fn ? fn(ammo, nullptr) : -1;
}
void ammo_set(void* ammo, int slot, int n) {
  if (!ammo_verified(ammo)) return;
  if (auto fn = vfn<ThisCallSetI>(ammo, slot)) fn(ammo, nullptr, n);
}
}  // namespace

int ammo_in_gun(void* ammo) { return ammo_call(ammo, kAmmoSlotGun); }
int ammo_carried(void* ammo) { return ammo_call(ammo, kAmmoSlotCarried); }
int ammo_max_in_gun(void* ammo) { return ammo_call(ammo, kAmmoSlotMaxGun); }
int ammo_max_carried(void* ammo) { return ammo_call(ammo, kAmmoSlotMaxCarried); }
bool ammo_infinite_carried(void* ammo) { return ammo_call(ammo, kAmmoSlotInfinite) == 1; }
void ammo_set_in_gun(void* ammo, int n) { ammo_set(ammo, kAmmoSlotSetGun, n); }
void ammo_set_carried(void* ammo, int n) { ammo_set(ammo, kAmmoSlotSetCarried, n); }

// --- slow-mo ------------------------------------------------------------------------------
bool slowmo_layout_ok(void* sm) { return slowmo_verified(sm) != nullptr; }
float slowmo_max(void* sm) {
  const SlowMoVt* v = slowmo_verified(sm);
  float f = 0.0f;
  return v && mem::read_safe(reinterpret_cast<uintptr_t>(sm) + v->off_max, &f) ? f : -1.0f;
}
float slowmo_current(void* sm) {
  const SlowMoVt* v = slowmo_verified(sm);
  float f = 0.0f;
  return v && mem::read_safe(reinterpret_cast<uintptr_t>(sm) + v->off_cur, &f) ? f : -1.0f;
}
void slowmo_set_current(void* sm, float value) {
  const SlowMoVt* v = slowmo_verified(sm);
  if (v) mem::write<float>(reinterpret_cast<uintptr_t>(sm) + v->off_cur, value);
}

// --- possession ------------------------------------------------------------------------------------
namespace {

struct PossessionVt { uintptr_t vtable; uint32_t off_state, off_time, off_remaining; int active_value; };
PossessionVt g_poss_ok[4] = {};
int g_poss_ok_n = 0;
uintptr_t g_poss_bad = 0;

struct PossessedVt { uintptr_t vtable; uint32_t off_max, off_remaining, off_started, off_active; };
PossessedVt g_victim_ok[4] = {};
int g_victim_ok_n = 0;
uintptr_t g_victim_bad = 0;

const PossessionVt* possession_verified(void* pc) {
  uintptr_t vt = 0;
  if (g_slot_poss_reset < 0 || !mem::read_safe(reinterpret_cast<uintptr_t>(pc), &vt) || !vt) return nullptr;
  for (int i = 0; i < g_poss_ok_n; ++i) if (g_poss_ok[i].vtable == vt) return &g_poss_ok[i];
  if (vt == g_poss_bad || g_poss_ok_n >= 4) return nullptr;
  // ResetTimer: cmp [ecx+state],4; jne; fldz; fcomp [ecx+time]; ...; fld [ecx+time]; fstp qword [ecx+remaining]
  const bool ok = slot_matches(pc, g_slot_poss_state, "8B 81 ?? ?? ?? ?? C3") &&
                  slot_matches(pc, g_slot_poss_victim, "8B 81 ?? ?? ?? ?? 85 C0 74 ?? 83 78 08 00 75 ?? 33 C0 C3 8B 81 ?? ?? ?? ?? C3") &&
                  slot_matches(pc, g_slot_poss_reset,
                               "83 B9 ?? ?? ?? ?? ?? 75 ?? D9 EE D8 99 ?? ?? ?? ?? DF E0 F6 C4 05 7A ?? D9 81 ?? ?? ?? ?? DD 99 ?? ?? ?? ??");
  if (!ok) { logf("ERROR: IPossessionComponent accessors have an unexpected shape - not touching possession"); g_poss_bad = vt; return nullptr; }
  const auto fstate = reinterpret_cast<uintptr_t>(vfn<void*>(pc, g_slot_poss_state));
  const auto freset = reinterpret_cast<uintptr_t>(vfn<void*>(pc, g_slot_poss_reset));
  PossessionVt v{vt, mem::read<uint32_t>(fstate + 2), mem::read<uint32_t>(freset + 13), mem::read<uint32_t>(freset + 32), mem::read<uint8_t>(freset + 6)};
  if (mem::read<uint32_t>(freset + 2) != v.off_state || mem::read<uint32_t>(freset + 26) != v.off_time) {
    logf("ERROR: IPossessionComponent ResetTimer disagrees with the state getter - not touching possession");
    g_poss_bad = vt;
    return nullptr;
  }
  g_poss_ok[g_poss_ok_n] = v;
  logf("IPossessionComponent layout verified (vtable exe+0x%X): state +0x%X (possessing = %d), time +0x%X, remaining +0x%X", rva(vt),
       v.off_state, v.active_value, v.off_time, v.off_remaining);
  return &g_poss_ok[g_poss_ok_n++];
}

const PossessedVt* possessed_verified(void* body) {
  uintptr_t vt = 0;
  if (g_slot_victim_max < 0 || !mem::read_safe(reinterpret_cast<uintptr_t>(body), &vt) || !vt) return nullptr;
  for (int i = 0; i < g_victim_ok_n; ++i) if (g_victim_ok[i].vtable == vt) return &g_victim_ok[i];
  if (vt == g_victim_bad || g_victim_ok_n >= 4) return nullptr;
  // activate(bool): cmp byte [ecx+active],0; mov al,[esp+4]; jne; test al,al; je; fld qword [clock]; fstp qword [ecx+started]; mov [ecx+active],al; ret 4
  const bool ok = slot_matches(body, kSlotVictimActivate, "80 B9 ?? ?? ?? ?? 00 8A 44 24 04 75 ?? 84 C0 74 ?? DD 05 ?? ?? ?? ?? DD 99 ?? ?? ?? ?? 88 81 ?? ?? ?? ?? C2 04 00") &&
                  slot_matches(body, kSlotVictimStartedAt, "DD 81 ?? ?? ?? ?? C3") &&
                  slot_matches(body, kSlotVictimPossessor, "8B 81 ?? ?? ?? ?? 85 C0 74 ?? 8B 40 08 85 C0 74 ?? 83 C0 E8 C3") &&
                  slot_matches(body, kSlotVictimReset, "D9 EE D8 99 ?? ?? ?? ?? DF E0 F6 C4 05 7A ?? D9 81 ?? ?? ?? ?? DD 99 ?? ?? ?? ?? C3") &&
                  slot_matches(body, g_slot_victim_max, "D9 81 ?? ?? ?? ?? C3");
  if (!ok) { logf("ERROR: IPossessedComponent accessors have an unexpected shape - not touching possession"); g_victim_bad = vt; return nullptr; }
  const auto fact = reinterpret_cast<uintptr_t>(vfn<void*>(body, kSlotVictimActivate));
  const auto fstart = reinterpret_cast<uintptr_t>(vfn<void*>(body, kSlotVictimStartedAt));
  const auto freset = reinterpret_cast<uintptr_t>(vfn<void*>(body, kSlotVictimReset));
  const auto fmax = reinterpret_cast<uintptr_t>(vfn<void*>(body, g_slot_victim_max));
  PossessedVt v{vt, mem::read<uint32_t>(fmax + 2), mem::read<uint32_t>(freset + 23), mem::read<uint32_t>(fact + 25), mem::read<uint32_t>(fact + 2)};
  const uintptr_t clock = mem::read<uint32_t>(fact + 19);
  const mem::Range whole = mem::module_range(reinterpret_cast<HMODULE>(g_base));
  if (mem::read<uint32_t>(freset + 4) != v.off_max || mem::read<uint32_t>(freset + 17) != v.off_max || mem::read<uint32_t>(fstart + 2) != v.off_started ||
      mem::read<uint32_t>(fact + 31) != v.off_active || !whole.contains(clock) || g_text.contains(clock)) {
    logf("ERROR: IPossessedComponent accessors disagree with each other - not touching possession");
    g_victim_bad = vt;
    return nullptr;
  }
  if (g_clock && g_clock != clock) { logf("ERROR: two different game clocks (exe+0x%X, exe+0x%X)", rva(g_clock), rva(clock)); g_victim_bad = vt; return nullptr; }
  g_clock = clock;
  g_victim_ok[g_victim_ok_n] = v;
  logf("IPossessedComponent layout verified (vtable exe+0x%X): max +0x%X, remaining +0x%X, started-at +0x%X, active +0x%X, clock exe+0x%X",
       rva(vt), v.off_max, v.off_remaining, v.off_started, v.off_active, rva(clock));
  return &g_victim_ok[g_victim_ok_n++];
}

}  // namespace

bool possession_layout_ok(void* pc) { return possession_verified(pc) != nullptr; }
int possession_state(void* pc) {
  const PossessionVt* v = possession_verified(pc);
  int st = -1;
  return v && mem::read_safe(reinterpret_cast<uintptr_t>(pc) + v->off_state, &st) ? st : -1;
}
bool possession_active(void* pc) {
  const PossessionVt* v = possession_verified(pc);
  return v && possession_state(pc) == v->active_value;
}
double possession_remaining(void* pc) {
  const PossessionVt* v = possession_verified(pc);
  double d = 0;
  return v && mem::read_safe(reinterpret_cast<uintptr_t>(pc) + v->off_remaining, &d) ? d : -1.0;
}
void* possession_victim(void* pc) {
  if (!possession_verified(pc)) return nullptr;
  auto fn = vfn<ThisCall0>(pc, g_slot_poss_victim);
  return fn ? fn(pc, nullptr) : nullptr;
}
void possession_reset_timer(void* pc) {
  if (!possession_verified(pc)) return;
  if (auto fn = vfn<ThisCall0>(pc, g_slot_poss_reset)) fn(pc, nullptr);
}
bool possessed_layout_ok(void* body) { return possessed_verified(body) != nullptr; }
float possessed_max_time(void* body) {
  const PossessedVt* v = possessed_verified(body);
  float f = 0;
  return v && mem::read_safe(reinterpret_cast<uintptr_t>(body) + v->off_max, &f) ? f : -1.0f;
}
double possessed_remaining(void* body) {
  const PossessedVt* v = possessed_verified(body);
  double d = 0;
  return v && mem::read_safe(reinterpret_cast<uintptr_t>(body) + v->off_remaining, &d) ? d : -1.0;
}
double possessed_started_at(void* body) {
  const PossessedVt* v = possessed_verified(body);
  double d = 0;
  return v && mem::read_safe(reinterpret_cast<uintptr_t>(body) + v->off_started, &d) ? d : -1.0;
}
void possessed_hold_start(void* body) {
  const PossessedVt* v = possessed_verified(body);
  double now = 0;
  if (v && g_clock && mem::read_safe(g_clock, &now)) mem::write<double>(reinterpret_cast<uintptr_t>(body) + v->off_started, now);
}
void* possessed_possessor_object(void* body) {
  if (!possessed_verified(body)) return nullptr;
  auto fn = vfn<ThisCall0>(body, kSlotVictimPossessor);
  return fn ? fn(body, nullptr) : nullptr;
}
double game_clock() {
  double now = 0;
  return g_clock && mem::read_safe(g_clock, &now) ? now : -1.0;
}

// --- message receivers ------------------------------------------------------------------------
bool hook_receiver(Receiver r, DispatchFn hook, DispatchFn* original) {
  if (r < 0 || r >= kReceiverCount) return false;
  ReceiverInfo& info = g_recv[r];
  if (info.hooked) { *original = info.original; return true; }
  if (!info.global) return false;
  // The game sets the receiver's vtable pointer lazily, the first time a
  // component of the class registers; hooking earlier would be undone.
  uint32_t guard = 0;
  if (!mem::read_safe(info.guard, &guard) || !(guard & 1)) return false;
  uintptr_t vp = 0;
  if (!mem::read_safe(info.global, &vp)) return false;
  if (vp != info.vtable) {
    if (!info.warned) { info.warned = true; logf("ERROR: receiver %s has vtable %p, expected exe+0x%X - not hooking", info.label, reinterpret_cast<void*>(vp), rva(info.vtable)); }
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    uintptr_t fn = 0;
    info.my_vtable[i] = mem::read_safe(info.vtable + i * 4, &fn) ? fn : 0;
  }
  if (!g_text.contains(info.my_vtable[0])) { logf("ERROR: receiver %s dispatcher is not in .text", info.label); return false; }
  info.original = reinterpret_cast<DispatchFn>(info.my_vtable[0]);
  info.my_vtable[0] = reinterpret_cast<uintptr_t>(hook);
  *original = info.original;  // before the swap: a delivery on another thread may hit the hook at once
  if (!mem::write<uintptr_t>(info.global, reinterpret_cast<uintptr_t>(info.my_vtable))) return false;
  info.hooked = true;
  logf("hooked receiver %s (dispatcher exe+0x%06X)", info.label, rva(reinterpret_cast<uintptr_t>(info.original)));
  return true;
}

bool receiver_hooked(Receiver r) { return r >= 0 && r < kReceiverCount && g_recv[r].hooked; }

void remove_hooks() {
  for (ReceiverInfo& info : g_recv) {
    if (!info.hooked) continue;
    uintptr_t vp = 0;
    if (mem::read_safe(info.global, &vp) && vp == reinterpret_cast<uintptr_t>(info.my_vtable)) mem::write<uintptr_t>(info.global, info.vtable);
    info.hooked = false;
  }
}

uintptr_t exe_base() { return g_base; }
const char* status_text() { return g_status; }

}  // namespace f3cc::game
