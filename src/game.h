#pragma once

#include <cstdint>

// The game side: finds the engine's script-facing wrappers by signature (for
// the vtable slots, component type ids and helper functions they encode), the
// live menu manager by its RTTI vtable, and wraps the handful of calls the
// cheats need. Nothing in here is an absolute address - everything is located
// in the running image and verified against the bytes it is read from.
namespace f3cc::game {

bool discover();          // signature scan; true when everything below is usable
bool ready();
bool context_ready();     // the menu manager has been found (a level is loaded)
void instance_loop();     // mod-thread loop that finds (and re-finds) the menu manager; never returns

// Only meant for the game's main thread (see dispatch.cpp) - these call into
// live engine objects.
bool pause_menu_showing();
bool level_started(int player);

// --- players and their characters ------------------------------------------
void* player(int index);             // the IPlayerComponent of a local player slot, or null
void* player_actor(void* player);    // its IActorComponent (the character it controls), or null
void* player_avatar(void* player);   // its own avatar game object, or null
void* actor_object(void* actor);     // the game object an IActorComponent belongs to
// The game's own "make this character indestructible": the actor component's
// setter when it has one, else an IndestructibleMessage sent to the object.
void set_indestructible(void* object, bool on, bool allow_damage);

// --- components ----------------------------------------------------------------
enum Type { kActor = 0, kAmmo, kSlowMo, kHitPointController, kHitPoint, kInventoryList, kPossession, kPossessed, kTypeCount };
const char* type_name(Type t);
void* get_component(void* object, Type t);                    // the first component of that type
int get_components(void* object, Type t, void** out, int max);  // all of them
// The complete object an interface pointer belongs to (MSVC RTTI), or null.
void* complete_object(void* iface);
const char* rtti_of(void* obj);  // short RTTI class name of a polymorphic object, "?" if unreadable

// Every game object carried in the object's inventory lists (weapons, grenades, ...).
int inventory_objects(void* object, void** out, int max);

// --- ammo (IAmmoComponent*) ----------------------------------------------------
// Verified once per session against the interface's own accessor bytes; the
// getters/setters no-op (or return -1) until then.
bool ammo_layout_ok(void* ammo);
int ammo_in_gun(void* ammo);
int ammo_carried(void* ammo);       // includes a shared ammo pool when the weapon uses one
int ammo_max_in_gun(void* ammo);
int ammo_max_carried(void* ammo);
bool ammo_infinite_carried(void* ammo);
void ammo_set_in_gun(void* ammo, int n);
void ammo_set_carried(void* ammo, int n);
int ammo_interface_offset();        // interface pointer - complete object (from RTTI)

// --- slow-mo (ISlowMoComponent*) --------------------------------------------------
bool slowmo_layout_ok(void* slowmo);
float slowmo_max(void* slowmo);
float slowmo_current(void* slowmo);
void slowmo_set_current(void* slowmo, float v);  // a field write: no message, no HUD flash

// --- possession (Fettel) -------------------------------------------------------------
// IPossessionComponent lives on Fettel's own object; IPossessedComponent on the
// body he is possessing. Both layouts are verified against their accessor bytes.
bool possession_layout_ok(void* possession);
bool possession_active(void* possession);       // the state ResetPossessionTimer requires
int possession_state(void* possession);
double possession_remaining(void* possession);  // seconds left as the game tracks them, for the log
void* possession_victim(void* possession);      // IPossessedComponent* of the body, or null
void possession_reset_timer(void* possession);  // the game's ResetPossessionTimer
bool possessed_layout_ok(void* possessed);
float possessed_max_time(void* possessed);
double possessed_remaining(void* possessed);
double possessed_started_at(void* possessed);   // game clock at possession start
void possessed_hold_start(void* possessed);     // started-at = now, so nothing measured from it elapses
void* possessed_possessor_object(void* possessed);  // Fettel's object, from the body's side
double game_clock();

// --- message receivers ----------------------------------------------------------
// Messages reach components through one global "receiver" object per
// (component class, message class); its single virtual is the dispatcher that
// calls the component's handler. Swapping that object's vtable pointer puts a
// hook in front of every delivery of that message to that class.
enum Receiver {
  kAmmoWeaponFire = 0,            // AmmoComponent <- WeaponFireMessage
  kBasicHitPointDamage,           // BasicHitPointComponent <- DamageMessage
  kHitPointControllerDamage,      // HitPointControllerComponent <- DamageMessage
  kLocationalHitPointControllerDamage,
  kReceiverCount
};
using DispatchFn = void(__fastcall*)(void* self, void* edx, void* component, void* handler, int adjust,
                                     void* message, void* flags);
// Installs the hook once the game has initialised the receiver (it does so
// lazily, on the first registration); returns true when in place. Cheap to
// retry every tick.
bool hook_receiver(Receiver r, DispatchFn hook, DispatchFn* original);
bool receiver_hooked(Receiver r);
void remove_hooks();

uintptr_t exe_base();
const char* status_text();

}  // namespace f3cc::game
