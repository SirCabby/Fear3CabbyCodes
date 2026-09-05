#pragma once

#include <cstdint>

// The four cheats. Their switches are shared between the render thread (the
// panel) and the game's main thread (dispatch.cpp, which applies them), so the
// switches are plain atomics and everything that touches the game runs from
// tick() on the main thread.
namespace f3cc::cheats {

enum Kind : int { kGodMode = 0, kInfiniteSlowMo, kInfinitePossession, kInfiniteAmmo, kOneHitKills, kCount };

const char* name(Kind k);
bool enabled(Kind k);
void set_enabled(Kind k, bool on);  // any thread; takes effect on the next tick

// What the panel shows next to the switches (written by tick, read anywhere).
struct Status {
  bool player_found = false;     // the local player's component exists
  bool object_found = false;     // its controlled game object exists
  bool slowmo_available = false; // the object has a slow-mo component (Point Man)
  bool possession_available = false;  // the player's object has a possession component (Fettel)
  bool possessing = false;       // Fettel is inside a body right now
  bool god_applied = false;      // the object has been told it is indestructible
  bool ammo_hooked = false;      // the weapon-fire hook is in place
  bool damage_hooked = false;    // the damage hooks are in place
  int ammo_items = 0;            // inventory items with ammo being held
  int hit_components = 0;        // hit-point components on the player's object
  char object_class[64] = {};    // RTTI name of the controlled object, for the log/panel
};
Status status();

// Main thread only.
void tick(bool in_level, int player_index);
void remove_hooks();  // teardown: put the receiver dispatchers back

}  // namespace f3cc::cheats
