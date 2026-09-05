// The mod ships as steam_api.dll, standing in front of the game's Steam API
// DLL (renamed steam_api_orig.dll by `make install`). Every function the
// original exports (53 of them; the exe imports 24) is a one-instruction thunk
// that jumps through a table of the real addresses - the stack is untouched,
// so the callee cleans up exactly as before, whatever its convention. The one
// data export (g_pSteamClientGameServer) is a PE forwarder in steam_api.def.
// The list of names lives in proxy_exports.inc, generated from the original
// DLL's export table by tools/gen_proxy.py; the .def aliases each name onto
// its thunk.
//
// Why steam_api and not binkw32 or fmodex: Fear3ChallengeGrant and
// Fear3TimeManager already proxy those two, and two proxies of one DLL cannot
// coexist. steam_api ships with the game and is not a Wine builtin, so it
// needs no WINEDLLOVERRIDES under Proton either - all three mods are pure
// drop-ins side by side. Steam's CEG wrapper in the exe talks to the real DLL
// through our thunks exactly as before; nothing is emulated.

#include "proxy.h"

#include <windows.h>

#include <cstdio>

#include "log.h"
#include "proxy_exports.inc"

// Plain C symbol so the assembly below can name it as _g_proxy_orig.
extern "C" {
void* g_proxy_orig[PROXY_EXPORT_COUNT] = {};
}

// One thunk per export: `jmp [g_proxy_orig + i*4]`, defined at file scope in
// assembly so no prologue/epilogue is ever emitted around the jump.
#define PROXY_THUNK(i, name)                                   \
  asm(".text\n"                                                \
      ".globl _steam_" #i "\n"                                 \
      "_steam_" #i ":\n"                                       \
      "\tjmp *_g_proxy_orig+" #i "*4\n");
PROXY_EXPORTS(PROXY_THUNK)
#undef PROXY_THUNK

namespace f3cc::proxy {

bool load_original(const char* dir) {
  char path[MAX_PATH] = {};
  std::snprintf(path, sizeof(path), "%ssteam_api_orig.dll", dir);

  HMODULE orig = LoadLibraryA(path);
  if (!orig) {
    logf("FATAL: could not load %s (GetLastError=%lu). Did `make install` run?", path,
         GetLastError());
    return false;
  }
  // Pin it: nothing may drop the last reference while the game still uses it.
  HMODULE pinned = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, path, &pinned);

  static const char* const kNames[] = {
#define PROXY_NAME(i, name) name,
      PROXY_EXPORTS(PROXY_NAME)
#undef PROXY_NAME
  };
  int missing = 0;
  for (int i = 0; i < PROXY_EXPORT_COUNT; ++i) {
    void* fn = reinterpret_cast<void*>(GetProcAddress(orig, kNames[i]));
    g_proxy_orig[i] = fn;
    if (!fn) {
      ++missing;
      logf("FATAL: %s lacks export %s", path, kNames[i]);
    }
  }
  if (missing) return false;
  logf("forwarding %d Steam API exports -> %s", PROXY_EXPORT_COUNT, path);
  return true;
}

}  // namespace f3cc::proxy
