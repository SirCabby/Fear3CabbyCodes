#include "cheats.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "game.h"
#include "log.h"
#include "mem.h"

namespace f3cc::cheats {
namespace {

volatile LONG g_enabled[kCount] = {};
Status g_status;
int g_player_index = 0;

void* g_object = nullptr;  // the game object the player's character is
bool g_god_applied = false;

// Ammo components held by the infinite-ammo cheat: the complete component (what
// the weapon-fire hook is handed) and its interface, plus the counts to hold.
struct Held {
  void* base = nullptr;
  void* ammo = nullptr;
  int gun = 0;
  int carried = 0;
  bool carried_stuck = false;  // the setter could not raise it (a shared pool); stop trying
};
constexpr int kMaxHeld = 32;
Held g_held[kMaxHeld];
volatile LONG g_held_n = 0;
DWORD g_last_walk = 0;
DWORD g_last_possession_trace = 0;

// Hit-point components on the player's object, for the damage hooks.
constexpr int kMaxHp = 16;
void* g_hp[kMaxHp] = {};
volatile LONG g_hp_n = 0;
DWORD g_last_hp = 0;

game::DispatchFn g_orig[game::kReceiverCount] = {};

const char* kNames[kCount] = {"God mode", "Infinite slo-mo", "Infinite possession", "Infinite ammo", "One hit kills"};

bool on(Kind k) { return g_enabled[k] != 0; }

Held* find_held(void* base) {
  const LONG n = g_held_n;
  for (LONG i = 0; i < n && i < kMaxHeld; ++i)
    if (g_held[i].base == base) return &g_held[i];
  return nullptr;
}

bool is_player_hp(void* component) {
  const LONG n = g_hp_n;
  for (LONG i = 0; i < n && i < kMaxHp; ++i)
    if (g_hp[i] == component) return true;
  return false;
}

// --- hooks ---------------------------------------------------------------------
// AmmoComponent::OnWeaponFire takes the shot's ammo out of the gun with
// SetAmmoInGun(gun - consumed); putting it straight back here means nothing
// downstream (the HUD, the empty-gun logic) ever sees the count drop.
void __fastcall hk_weapon_fire(void* self, void* edx, void* component, void* handler, int adjust, void* message,
                               void* flags) {
  g_orig[game::kAmmoWeaponFire](self, edx, component, handler, adjust, message, flags);
  if (!on(kInfiniteAmmo)) return;
  Held* h = find_held(component);
  if (!h) return;
  const int gun = game::ammo_in_gun(h->ammo);
  if (gun >= 0 && gun < h->gun) game::ammo_set_in_gun(h->ammo, h->gun);
}

// DamageMessage: {vtable, DamageInfo} with the info at +4: {flags(u8, bit 1 =
// kill outright), +4 source type, +8 amount(float), +0xc dealer object handle,
// +0x10 dealer player index (-1 = not a player), ...}. The same message object
// visits every receiver on the target, so editing it once is enough.
void damage(game::Receiver r, void* self, void* edx, void* component, void* handler, int adjust, void* message,
            void* flags) {
  const bool god = on(kGodMode), ohk = on(kOneHitKills);
  if ((god || ohk) && message && mem::readable(message, 0x18)) {
    uint8_t* info = static_cast<uint8_t*>(message) + 4;
    float* amount = reinterpret_cast<float*>(info + 8);
    const int dealer = *reinterpret_cast<int*>(info + 0x10);
    const bool target_is_player = is_player_hp(component);
    if (god && target_is_player) {
      *amount = 0.0f;
      info[0] &= ~uint8_t(2);
    } else if (ohk && !target_is_player && dealer == g_player_index && *amount > 0.0f &&
               *amount < config::get().one_hit_damage) {
      *amount = config::get().one_hit_damage;
    }
  }
  g_orig[r](self, edx, component, handler, adjust, message, flags);
}

void __fastcall hk_damage_basic(void* self, void* edx, void* c, void* h, int a, void* m, void* f) {
  damage(game::kBasicHitPointDamage, self, edx, c, h, a, m, f);
}
void __fastcall hk_damage_controller(void* self, void* edx, void* c, void* h, int a, void* m, void* f) {
  damage(game::kHitPointControllerDamage, self, edx, c, h, a, m, f);
}
void __fastcall hk_damage_locational(void* self, void* edx, void* c, void* h, int a, void* m, void* f) {
  damage(game::kLocationalHitPointControllerDamage, self, edx, c, h, a, m, f);
}

bool ensure_hooks(bool* ammo, bool* dmg) {
  *ammo = game::hook_receiver(game::kAmmoWeaponFire, &hk_weapon_fire, &g_orig[game::kAmmoWeaponFire]);
  const bool b = game::hook_receiver(game::kBasicHitPointDamage, &hk_damage_basic, &g_orig[game::kBasicHitPointDamage]);
  const bool c = game::hook_receiver(game::kHitPointControllerDamage, &hk_damage_controller, &g_orig[game::kHitPointControllerDamage]);
  game::hook_receiver(game::kLocationalHitPointControllerDamage, &hk_damage_locational, &g_orig[game::kLocationalHitPointControllerDamage]);
  *dmg = b && c;
  return *ammo && *dmg;
}

// --- per-tick work -------------------------------------------------------------
void refresh_hp(void* object) {
  void* list[kMaxHp];
  int n = 0;
  if (void* hpc = game::get_component(object, game::kHitPointController))
    if (void* base = game::complete_object(hpc)) list[n++] = base;
  void* ifaces[kMaxHp];
  const int m = game::get_components(object, game::kHitPoint, ifaces, kMaxHp);
  for (int i = 0; i < m && n < kMaxHp; ++i)
    if (void* base = game::complete_object(ifaces[i])) list[n++] = base;
  bool changed = n != g_hp_n;
  for (int i = 0; i < n && !changed; ++i) changed = g_hp[i] != list[i];
  if (changed) {
    g_hp_n = 0;
    for (int i = 0; i < n; ++i) g_hp[i] = list[i];
    g_hp_n = n;
    if (config::get().trace) logf("trace: %d hit-point component(s) on the player's object", n);
  }
}

// Every inventory object with an ammo component: fill it up the first time it
// is seen with the cheat on, then hold the counts (a ratchet: they may go up
// with pickups, never down).
void walk_ammo(void* object) {
  void* items[64];
  const int n = game::inventory_objects(object, items, 64);
  Held next[kMaxHeld];
  int nn = 0;
  for (int i = 0; i < n && nn < kMaxHeld; ++i) {
    void* ammo = game::get_component(items[i], game::kAmmo);
    if (!ammo || !game::ammo_layout_ok(ammo)) continue;
    void* base = game::complete_object(ammo);
    if (!base) continue;
    Held h;
    h.base = base;
    h.ammo = ammo;
    if (Held* old = find_held(base)) {
      h = *old;
    } else {
      // First sight: fill to the weapon's own maxima.
      const int max_gun = game::ammo_max_in_gun(ammo), max_carried = game::ammo_max_carried(ammo);
      const int gun = game::ammo_in_gun(ammo), carried = game::ammo_carried(ammo);
      if (max_gun > gun) game::ammo_set_in_gun(ammo, max_gun);
      if (!game::ammo_infinite_carried(ammo) && max_carried > carried) game::ammo_set_carried(ammo, max_carried);
      h.gun = game::ammo_in_gun(ammo);
      h.carried = game::ammo_carried(ammo);
      logf("holding ammo on %s: gun %d -> %d (max %d), carried %d -> %d (max %d%s)", game::rtti_of(items[i]), gun, h.gun,
           max_gun, carried, h.carried, max_carried, game::ammo_infinite_carried(ammo) ? ", infinite" : "");
    }
    next[nn++] = h;
  }
  g_held_n = 0;
  for (int i = 0; i < nn; ++i) g_held[i] = next[i];
  g_held_n = nn;
}

void hold_ammo() {
  const LONG n = g_held_n;
  for (LONG i = 0; i < n; ++i) {
    Held& h = g_held[i];
    const int gun = game::ammo_in_gun(h.ammo);
    if (gun < 0) continue;
    if (gun < h.gun) game::ammo_set_in_gun(h.ammo, h.gun);
    else h.gun = gun;
    const int carried = game::ammo_carried(h.ammo);
    if (carried < 0) continue;
    if (carried < h.carried) {
      if (!h.carried_stuck) {
        game::ammo_set_carried(h.ammo, h.carried);
        if (game::ammo_carried(h.ammo) < h.carried) {
          h.carried_stuck = true;
          logf("carried ammo on a weapon could not be raised from %d to %d (shared pool?) - holding the gun only",
               carried, h.carried);
        }
      }
    } else {
      h.carried = carried;
    }
  }
}

void forget_object(const char* why) {
  if (g_object || g_held_n || g_hp_n) {
    if (config::get().trace) logf("trace: %s - forgetting the player's object", why);
  }
  g_object = nullptr;
  g_god_applied = false;
  g_held_n = 0;
  g_hp_n = 0;
}

}  // namespace

const char* name(Kind k) { return k >= 0 && k < kCount ? kNames[k] : "?"; }
bool enabled(Kind k) { return k >= 0 && k < kCount && g_enabled[k] != 0; }
void set_enabled(Kind k, bool on) {
  if (k >= 0 && k < kCount) InterlockedExchange(&g_enabled[k], on ? 1 : 0);
}
Status status() { return g_status; }

void tick(bool in_level, int player_index) {
  static bool init = false;
  if (!init) {
    init = true;
    const config::Settings& c = config::get();
    set_enabled(kGodMode, c.god_mode);
    set_enabled(kInfiniteSlowMo, c.infinite_slowmo);
    set_enabled(kInfinitePossession, c.infinite_possession);
    set_enabled(kInfiniteAmmo, c.infinite_ammo);
    set_enabled(kOneHitKills, c.one_hit_kills);
  }
  g_player_index = player_index;
  Status st;
  ensure_hooks(&st.ammo_hooked, &st.damage_hooked);

  if (!in_level) {
    forget_object("level ended");
    g_status = st;
    return;
  }
  void* p = game::player(player_index);
  st.player_found = p != nullptr;
  void* actor = p ? game::player_actor(p) : nullptr;
  void* obj = actor ? game::actor_object(actor) : nullptr;
  void* avatar_obj = p ? game::player_avatar(p) : nullptr;
  if (!obj) obj = avatar_obj;
  if (!avatar_obj) avatar_obj = obj;
  st.object_found = obj != nullptr;
  if (obj != g_object) {
    forget_object("character changed");
    g_object = obj;
    if (obj) logf("player %d character object %p (%s)%s", player_index, obj, game::rtti_of(obj), actor ? "" : " (avatar, no actor component)");
  }
  if (!obj) {
    g_status = st;
    return;
  }
  std::snprintf(st.object_class, sizeof(st.object_class), "%s", game::rtti_of(obj));
  const DWORD now = GetTickCount();

  // God mode: the game's own indestructible state, re-sent when the object changes.
  const bool god = on(kGodMode);
  if (god != g_god_applied) {
    game::set_indestructible(obj, god, false);
    g_god_applied = god;
    logf("god mode %s on %p", god ? "applied" : "removed", obj);
  }
  st.god_applied = g_god_applied;
  if (god || on(kOneHitKills) || now - g_last_hp > 1000) {
    if (now - g_last_hp > 500 || g_hp_n == 0) {
      refresh_hp(obj);
      g_last_hp = now;
    }
  }
  st.hit_components = g_hp_n;

  // Infinite slo-mo: hold the meter at its maximum (a field write, so no
  // recharge message is spammed while the meter drains).
  void* sm = game::get_component(obj, game::kSlowMo);
  st.slowmo_available = sm && game::slowmo_layout_ok(sm);
  if (on(kInfiniteSlowMo) && st.slowmo_available) {
    const float mx = game::slowmo_max(sm), cur = game::slowmo_current(sm);
    if (mx > 0.0f && cur < mx) game::slowmo_set_current(sm, mx);
  }

  // Infinite possession (Fettel): the possession component sits on his own object,
  // which is the avatar while he is inside someone else; fall back to the body's
  // link back to its possessor. While possessing, call the game's own
  // ResetPossessionTimer every tick and hold the body's started-at clock, so
  // neither way of measuring the possession ever runs out.
  void* pc = game::get_component(avatar_obj, game::kPossession);
  if (!pc && obj != avatar_obj) pc = game::get_component(obj, game::kPossession);
  if (!pc) {
    if (void* body = game::get_component(obj, game::kPossessed))
      if (void* owner = game::possessed_possessor_object(body)) pc = game::get_component(owner, game::kPossession);
  }
  st.possession_available = pc && game::possession_layout_ok(pc);
  if (st.possession_available) {
    st.possessing = game::possession_active(pc);
    if (on(kInfinitePossession) && st.possessing) {
      game::possession_reset_timer(pc);
      void* body = game::possession_victim(pc);
      if (body && game::possessed_layout_ok(body)) game::possessed_hold_start(body);
      if (config::get().trace && now - g_last_possession_trace > 1000) {
        g_last_possession_trace = now;
        logf("trace: possessing: state %d, remaining %.2f s; body %p max %.1f remaining %.2f started-at %.2f, clock %.2f",
             game::possession_state(pc), game::possession_remaining(pc), body, body ? game::possessed_max_time(body) : -1.0f,
             body ? game::possessed_remaining(body) : -1.0, body ? game::possessed_started_at(body) : -1.0, game::game_clock());
      }
    }
  }

  // Infinite ammo: walk the inventory now and then (weapons come and go), hold every tick.
  if (on(kInfiniteAmmo)) {
    if (now - g_last_walk > 250 || g_held_n == 0) {
      walk_ammo(obj);
      g_last_walk = now;
    }
    hold_ammo();
  } else if (g_held_n) {
    g_held_n = 0;
    logf("infinite ammo off - released %s", "the held weapons");
  }
  st.ammo_items = g_held_n;
  g_status = st;
}

void remove_hooks() { game::remove_hooks(); }

}  // namespace f3cc::cheats
