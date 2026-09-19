#include "command_input.hpp"

#include "../sdl_platform.hpp"
#include "preferences.hpp"

namespace game {
namespace {

// The installed table, or null for the built-in defaults. Written once by
// NovaApp_Run; Key Settings mutates the table through the pointer, never this.
const KeyBindings *g_command_bindings = nullptr;

[[nodiscard]] const KeyBindings &CurrentBindings() {
  if (g_command_bindings != nullptr) {
    return *g_command_bindings;
  }
  // NovaPrefs_ResetKeyBindings (0x004b4400) values. A function-local static so
  // queries before startup, and tests without a runtime, still resolve the
  // documented default keys.
  static const KeyBindings defaults = [] {
    KeyBindings bindings;
    bindings.ResetToDefaults();
    return bindings;
  }();
  return defaults;
}

} // namespace

void NovaInput_InstallCommandBindings(const KeyBindings *bindings) {
  g_command_bindings = bindings;
}

std::uint16_t NovaInput_CommandKey(std::size_t command) {
  const auto &bindings = CurrentBindings();
  if (command >= bindings.cmd_to_key.size()) {
    return 0xffff;
  }
  const std::uint16_t key = bindings.cmd_to_key[command];
  return (key == 0xff || key == 0xffff) ? 0xffff : key;
}

// Ghidra 0x00469ca0 NovaInput_IsCommandActiveWithGameplayGuards (partial: the
// command-id -> bound-key -> held-key path; the DAT_00596d3c modal guard and
// the g_panel_suppressed_commands list are unmodeled).
bool NovaInput_IsCommandActive(SdlPlatform &platform, std::size_t command) {
  const std::uint16_t key = NovaInput_CommandKey(command);
  return key != 0xffff && platform.IsOriginalKeyCodeHeld(key);
}

} // namespace game
