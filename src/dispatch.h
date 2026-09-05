#pragma once

// The main-thread tick. Everything that talks to the engine runs on the thread
// that pumps window messages (the game renders and updates on that same
// thread): an import-table hook on PeekMessageA gives a callback there once
// per pump, and the cheats are applied from it.
namespace f3cc::dispatch {

bool install();  // from DllMain (import-table patch only); records the primary thread
void uninstall();

struct Snapshot {
  bool game_ready = false;
  bool context_ready = false;  // the menu manager has been found
  bool paused = false;         // the game's pause menu is up, in a level
  bool in_level = false;
  bool show_panel = false;     // the visibility rule, evaluated on the main thread
  unsigned long main_thread = 0;
  unsigned long long ticks = 0;
};
Snapshot snapshot();

}  // namespace f3cc::dispatch
