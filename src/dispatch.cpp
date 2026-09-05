#include "dispatch.h"

#include <windows.h>

#include "cheats.h"
#include "config.h"
#include "game.h"
#include "log.h"
#include "mem.h"

namespace f3cc::dispatch {
namespace {

using PeekMessageAFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
PeekMessageAFn g_orig_peek = nullptr;
DWORD g_main_thread = 0;  // the process's primary thread: DllMain(PROCESS_ATTACH) runs on it

Snapshot g_snap;
DWORD g_last_tick = 0;
bool g_tick_logged = false;
bool g_paused_prev = false;
bool g_in_level_prev = false;

void tick() {
  const DWORD now = GetTickCount();
  if (now - g_last_tick < 16) return;  // the pump spins many times per frame
  g_last_tick = now;

  Snapshot s;
  s.main_thread = GetCurrentThreadId();
  s.ticks = g_snap.ticks + 1;
  s.game_ready = game::ready();
  if (!g_tick_logged) {
    g_tick_logged = true;
    logf("main-thread tick running on thread %lu", s.main_thread);
  }
  if (!s.game_ready) {
    g_snap = s;
    return;
  }
  const int player = config::get().player_index;
  s.context_ready = game::context_ready();
  s.in_level = game::level_started(player);
  s.paused = s.in_level && game::pause_menu_showing();

  if (s.in_level != g_in_level_prev) {
    logf("level %s (player %d)", s.in_level ? "started" : "ended", player);
    g_in_level_prev = s.in_level;
  }
  if (s.paused != g_paused_prev) {
    if (config::get().trace) logf("pause menu %s", s.paused ? "opened" : "closed");
    g_paused_prev = s.paused;
  }

  // The cheats run every tick, paused or not: holds must be re-asserted every
  // frame, and a switch flipped in the panel takes effect the moment the game
  // resumes (or immediately, for the ones that only write fields).
  cheats::tick(s.in_level, player);

  s.show_panel = s.paused && s.in_level;
  g_snap = s;
}

BOOL WINAPI hk_peek_message(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove) {
  // Other threads pump messages too (the Steam overlay, worker windows); the
  // game runs on the primary thread, and so must everything that calls into it.
  if (GetCurrentThreadId() == g_main_thread) tick();
  return g_orig_peek(msg, hwnd, min, max, remove);
}

}  // namespace

bool install() {
  g_main_thread = GetCurrentThreadId();
  void* prev = mem::iat_hook(GetModuleHandleA(nullptr), "USER32.dll", "PeekMessageA",
                             reinterpret_cast<void*>(&hk_peek_message));
  if (!prev) {
    logf("ERROR: could not hook PeekMessageA in the exe's import table - no main-thread tick");
    return false;
  }
  g_orig_peek = reinterpret_cast<PeekMessageAFn>(prev);
  logf("dispatch: PeekMessageA import hooked (original %p)", prev);
  return true;
}

void uninstall() {
  if (!g_orig_peek) return;
  mem::iat_hook(GetModuleHandleA(nullptr), "USER32.dll", "PeekMessageA",
                reinterpret_cast<void*>(g_orig_peek));
  g_orig_peek = nullptr;
}

Snapshot snapshot() { return g_snap; }

}  // namespace f3cc::dispatch
