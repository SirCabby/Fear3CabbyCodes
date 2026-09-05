#include "overlay.h"

#include <cstdio>
#include <cstring>

#include "cheats.h"
#include "config.h"
#include "dispatch.h"
#include "game.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "log.h"
#include "mem.h"
#include "version.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace f3cc::overlay {
namespace {

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn g_orig_gpa = nullptr;

Backend g_backend = Backend::kNone;
HWND g_hwnd = nullptr;
WNDPROC g_orig_wndproc = nullptr;
bool g_context_ready = false;
bool g_visible = false;
bool g_user_hidden = false;  // the toggle key, while the pause menu is up

FARPROC WINAPI hk_get_proc_address(HMODULE module, LPCSTR name) {
  FARPROC real = g_orig_gpa(module, name);
  if (!real || !name || !HIWORD(reinterpret_cast<uintptr_t>(name))) return real;
  if (FARPROC w = dx11::wrap(name, real)) return w;
  if (FARPROC w = dx9::wrap(name, real)) return w;
  return real;
}

LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  const dispatch::Snapshot s = dispatch::snapshot();
  const bool showable = s.show_panel || config::get().always_show;
  // F-keys above F9 arrive as WM_SYSKEYDOWN; accept both, ignore auto-repeat.
  if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lp & (1 << 30)) &&
      static_cast<int>(wp) == config::get().toggle_key && showable) {
    g_user_hidden = !g_user_hidden;
    logf("panel %s by the toggle key", g_user_hidden ? "hidden" : "shown");
    return 0;
  }
  // Input only reaches ImGui while the panel is up (the pause menu): that is
  // when the cursor is free. During play the game keeps every key and click.
  if (g_visible && g_context_ready) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    const ImGuiIO& io = ImGui::GetIO();
    // Swallow only what ImGui is using: clicks on the panel. Everything else -
    // Escape to resume, clicks on the game's own pause menu - goes through.
    if (io.WantCaptureMouse && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return 0;
    if (io.WantTextInput && msg >= WM_KEYFIRST && msg <= WM_KEYLAST && wp != VK_ESCAPE) return 0;
  }
  return CallWindowProcA(g_orig_wndproc, hwnd, msg, wp, lp);
}

void cheat_row(cheats::Kind k, const char* label, const char* help, bool available, const char* unavailable_text) {
  bool on = cheats::enabled(k);
  if (!available) ImGui::BeginDisabled();
  if (ImGui::Checkbox(label, &on)) {
    cheats::set_enabled(k, on);
    logf("%s %s (panel)", cheats::name(k), on ? "ON" : "OFF");
  }
  if (!available) ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(360.0f);
    ImGui::TextUnformatted(help);
    if (!available && unavailable_text) {
      ImGui::Separator();
      ImGui::TextUnformatted(unavailable_text);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  if (!available && unavailable_text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", unavailable_text);
  }
}

}  // namespace

bool claim(Backend b) {
  if (g_backend == Backend::kNone) {
    g_backend = b;
    logf("overlay: %s is the active renderer", b == Backend::kDx11 ? "D3D11" : "D3D9");
  }
  return g_backend == b;
}

bool ensure_context(HWND hwnd) {
  if (g_context_ready) return true;
  if (!hwnd) return false;
  g_hwnd = hwnd;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  // Window position persists beside the DLL, under the mod's own name rather
  // than a stray imgui.ini in the game folder.
  static char ini_path[MAX_PATH] = {};
  std::snprintf(ini_path, sizeof(ini_path), "%sFear3CabbyCodes.imgui.ini", config::dir());
  io.IniFilename = ini_path;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::StyleColorsDark();
  ImGui::GetStyle().WindowRounding = 4.0f;

  if (!ImGui_ImplWin32_Init(hwnd)) {
    logf("ERROR: ImGui Win32 backend init failed");
    ImGui::DestroyContext();
    return false;
  }
  g_orig_wndproc = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hk_wndproc)));
  g_context_ready = true;
  logf("overlay ready (hwnd=%p, render thread %lu)", hwnd, GetCurrentThreadId());
  return true;
}

bool wants_draw() {
  const dispatch::Snapshot s = dispatch::snapshot();
  const bool showable = s.show_panel || config::get().always_show;
  if (!showable) g_user_hidden = false;  // the next pause starts visible again
  g_visible = g_context_ready && showable && !g_user_hidden;
  ImGui::GetIO().MouseDrawCursor = g_visible;  // the game may hide the OS cursor
  return g_visible;
}

void draw_panel() {
  const dispatch::Snapshot s = dispatch::snapshot();
  const cheats::Status st = cheats::status();
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 400, 60), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("F.E.A.R. 3 - Cabby Codes  v" F3CC_VERSION, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }

  if (!s.game_ready) {
    ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "Game hooks: %s", game::status_text());
    ImGui::End();
    return;
  }
  const bool have_object = st.player_found && st.object_found;
  if (!have_object) {
    ImGui::TextDisabled(st.player_found ? "Waiting for the player's character..."
                                        : "Waiting for the player (a level must be loaded)...");
    ImGui::Separator();
  }

  ImGui::BeginDisabled(!have_object);
  cheat_row(cheats::kGodMode, "God mode",
            "You cannot die and take no damage. Uses the game's own 'indestructible' state (the one cutscenes "
            "use), re-applied whenever your character changes.",
            true, nullptr);
  cheat_row(cheats::kInfiniteSlowMo, "Infinite slo-mo",
            "The slow-mo meter is held full, so it never runs out. Point Man only: Fettel has no slow-mo.",
            st.slowmo_available || !have_object, have_object ? "Point Man only" : nullptr);
  cheat_row(cheats::kInfinitePossession, "Infinite possession",
            "A possessed body never times out: the possession timer is reset every frame with the game's own "
            "ResetPossessionTimer. Fettel only: Point Man cannot possess.",
            st.possession_available || !have_object, have_object ? "Fettel only" : nullptr);
  cheat_row(cheats::kInfiniteAmmo, "Infinite ammo",
            "Every weapon you carry - the equipped one and grenades included - is filled up, and the counts "
            "never go down: each shot is put straight back.",
            true, nullptr);
  cheat_row(cheats::kOneHitKills, "One hit kills",
            "Any damage you deal is raised to a lethal amount. Enemies with scripted invulnerability phases "
            "keep those phases; everything else dies on the first hit.",
            true, nullptr);
  ImGui::EndDisabled();

  ImGui::Separator();
  if (have_object) {
    ImGui::TextDisabled("Character: %s", st.object_class[0] ? st.object_class : "?");
    ImGui::TextDisabled("%d weapon(s) held  |  %d hit-point component(s)", st.ammo_items, st.hit_components);
  }
  const bool hooks_ok = st.ammo_hooked && st.damage_hooked;
  if (!hooks_ok && s.game_ready)
    ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "Hooks: ammo %s, damage %s - see the log",
                       st.ammo_hooked ? "ok" : "MISSING", st.damage_hooked ? "ok" : "MISSING");
  ImGui::TextDisabled("F%d hides this panel", config::get().toggle_key - 0x6F);
  ImGui::End();
}

void shutdown_imgui() {
  if (!g_context_ready) return;
  ImGui_ImplWin32_Shutdown();
  if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
  g_context_ready = false;
  g_visible = false;
}

bool install() {
  void* prev = mem::iat_hook(GetModuleHandleA(nullptr), "KERNEL32.dll", "GetProcAddress",
                             reinterpret_cast<void*>(&hk_get_proc_address));
  if (!prev) {
    logf("ERROR: could not hook GetProcAddress in the exe's import table - no overlay");
    return false;
  }
  g_orig_gpa = reinterpret_cast<GetProcAddressFn>(prev);
  logf("overlay: GetProcAddress import hooked (original %p)", prev);
  return true;
}

void uninstall() {
  // Window procedure first: a message arriving after our image is gone would
  // jump into freed memory.
  if (g_orig_wndproc && g_hwnd && IsWindow(g_hwnd)) {
    SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wndproc));
    g_orig_wndproc = nullptr;
  }
  dx11::uninstall();
  dx9::uninstall();
  shutdown_imgui();
  if (g_orig_gpa) {
    mem::iat_hook(GetModuleHandleA(nullptr), "KERNEL32.dll", "GetProcAddress",
                  reinterpret_cast<void*>(g_orig_gpa));
    g_orig_gpa = nullptr;
  }
  logf("overlay hooks removed");
}

}  // namespace f3cc::overlay
