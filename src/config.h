#pragma once

namespace f3cc::config {

struct Settings {
  int toggle_key = 0x76;      // VK_F7: hide/show the panel while the pause menu is up
  int player_index = 0;       // local player slot the cheats apply to
  // Initial state of each cheat when the game starts; the panel changes them
  // for the session.
  bool god_mode = false;
  bool infinite_slowmo = false;
  bool infinite_possession = false;
  bool infinite_ammo = false;
  bool one_hit_kills = false;
  float one_hit_damage = 1000000.0f;  // the damage a player hit is raised to
  bool trace = false;         // verbose diagnostics
  bool always_show = false;   // debug: draw the panel outside the pause menu too
  // "Disable = overlay,dispatch,game" turns whole subsystems off so a fault can
  // be bisected to one of them.
  bool disable_overlay = false;
  bool disable_dispatch = false;
  bool disable_game = false;
};

const Settings& get();

// Fear3CabbyCodes.ini next to the DLL. Written with documented defaults the
// first time so the options are discoverable.
void load(const char* dir);
const char* dir();  // the DLL's directory, with a trailing separator

}  // namespace f3cc::config
