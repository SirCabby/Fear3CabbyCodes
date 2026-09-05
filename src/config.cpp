#include "config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"

namespace f3cc::config {
namespace {

Settings g_settings;
char g_path[MAX_PATH] = {};
char g_dir[MAX_PATH] = {};

bool truthy(const char* v) {
  return !_stricmp(v, "1") || !_stricmp(v, "on") || !_stricmp(v, "true") || !_stricmp(v, "yes");
}

void write_defaults() {
  FILE* f = std::fopen(g_path, "w");
  if (!f) return;
  std::fprintf(f,
               "# Fear3CabbyCodes\n"
               "#\n"
               "#   ToggleKey      = 0x76   ; virtual-key code that hides/shows the panel while paused (0x76 = F7)\n"
               "#   PlayerIndex    = 0      ; local player slot the cheats apply to\n"
               "#   GodMode        = 0      ; cheats switched on when the game starts (the panel changes\n"
               "#   InfiniteSlowMo = 0      ; them for the session)\n"
               "#   InfinitePossession = 0\n"
               "#   InfiniteAmmo   = 0\n"
               "#   OneHitKills    = 0\n"
               "#   OneHitDamage   = 1000000 ; damage every hit you land is raised to with One hit kills\n"
               "#   AlwaysShow     = 0      ; debug: draw the panel outside the pause menu too\n"
               "#   Trace          = 0      ; verbose diagnostics in Fear3CabbyCodes.log\n"
               "#   Disable        =        ; comma list of subsystems to turn off: overlay,dispatch,game\n"
               "ToggleKey = 0x76\n"
               "PlayerIndex = 0\n"
               "GodMode = 0\n"
               "InfiniteSlowMo = 0\n"
               "InfinitePossession = 0\n"
               "InfiniteAmmo = 0\n"
               "OneHitKills = 0\n"
               "OneHitDamage = 1000000\n"
               "AlwaysShow = 0\n"
               "Trace = 0\n"
               "Disable =\n");
  std::fclose(f);
}

}  // namespace

const Settings& get() { return g_settings; }
const char* dir() { return g_dir; }

void load(const char* dir) {
  std::snprintf(g_dir, sizeof(g_dir), "%s", dir);
  std::snprintf(g_path, sizeof(g_path), "%sFear3CabbyCodes.ini", dir);
  FILE* f = std::fopen(g_path, "r");
  if (!f) {
    write_defaults();
    logf("config: no %s - wrote one with the defaults", g_path);
    return;
  }
  char line[256];
  while (std::fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#' || *p == ';' || *p == '[' || *p == '\n' || *p == '\r' || !*p) continue;
    char* eq = std::strchr(p, '=');
    if (!eq) continue;
    *eq = '\0';
    char* key = p;
    char* val = eq + 1;
    for (char* e = key + std::strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t');) *--e = '\0';
    while (*val == ' ' || *val == '\t') ++val;
    for (char* e = val + std::strlen(val);
         e > val && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t');)
      *--e = '\0';

    if (!_stricmp(key, "ToggleKey")) {
      int v = static_cast<int>(std::strtol(val, nullptr, 0));
      if (v > 0 && v < 256) g_settings.toggle_key = v;
    } else if (!_stricmp(key, "PlayerIndex")) {
      int v = static_cast<int>(std::strtol(val, nullptr, 0));
      if (v >= 0 && v < 4) g_settings.player_index = v;
    } else if (!_stricmp(key, "GodMode")) {
      g_settings.god_mode = truthy(val);
    } else if (!_stricmp(key, "InfiniteSlowMo")) {
      g_settings.infinite_slowmo = truthy(val);
    } else if (!_stricmp(key, "InfinitePossession")) {
      g_settings.infinite_possession = truthy(val);
    } else if (!_stricmp(key, "InfiniteAmmo")) {
      g_settings.infinite_ammo = truthy(val);
    } else if (!_stricmp(key, "OneHitKills")) {
      g_settings.one_hit_kills = truthy(val);
    } else if (!_stricmp(key, "OneHitDamage")) {
      float v = static_cast<float>(std::strtod(val, nullptr));
      if (v >= 1.0f && v <= 1.0e9f) g_settings.one_hit_damage = v;
    } else if (!_stricmp(key, "AlwaysShow")) {
      g_settings.always_show = truthy(val);
    } else if (!_stricmp(key, "Trace")) {
      g_settings.trace = truthy(val);
    } else if (!_stricmp(key, "Disable")) {
      g_settings.disable_overlay = std::strstr(val, "overlay") != nullptr;
      g_settings.disable_dispatch = std::strstr(val, "dispatch") != nullptr;
      g_settings.disable_game = std::strstr(val, "game") != nullptr;
    } else {
      logf("config: unknown key '%s' ignored", key);
    }
  }
  std::fclose(f);
  logf("config: ToggleKey=0x%02X PlayerIndex=%d GodMode=%d InfiniteSlowMo=%d InfinitePossession=%d InfiniteAmmo=%d "
       "OneHitKills=%d OneHitDamage=%.0f AlwaysShow=%d Trace=%d",
       g_settings.toggle_key, g_settings.player_index, g_settings.god_mode, g_settings.infinite_slowmo,
       g_settings.infinite_possession, g_settings.infinite_ammo, g_settings.one_hit_kills, g_settings.one_hit_damage,
       g_settings.always_show, g_settings.trace);
}

}  // namespace f3cc::config
