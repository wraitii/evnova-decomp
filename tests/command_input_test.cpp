#include "game/command_input.hpp"

#include <catch2/catch_test_macros.hpp>

#include "game/preferences.hpp"

using game::KeyBindings;
using game::NovaInput_CommandKey;
using game::NovaInput_InstallCommandBindings;

TEST_CASE("command table defaults resolve the documented starmap key") {
  NovaInput_InstallCommandBindings(nullptr);
  // Slot 9 is the map command; NovaPrefs_ResetKeyBindings (0x004b4400) writes
  // 0x32 (M).
  CHECK(NovaInput_CommandKey(0x09) == 0x32);
  // Slot 0x21 ships unbound.
  CHECK(NovaInput_CommandKey(0x21) == 0xffff);
}

TEST_CASE("an installed table overrides the defaults") {
  KeyBindings bindings;
  bindings.ResetToDefaults();
  bindings.cmd_to_key[0x09] = 0x1e; // A
  NovaInput_InstallCommandBindings(&bindings);
  CHECK(NovaInput_CommandKey(0x09) == 0x1e);
  // A raw 0xff in the table is normalized to the unbound sentinel.
  bindings.cmd_to_key[0x09] = 0xff;
  CHECK(NovaInput_CommandKey(0x09) == 0xffff);
  NovaInput_InstallCommandBindings(nullptr);
}

TEST_CASE("out-of-range command slots report unbound") {
  NovaInput_InstallCommandBindings(nullptr);
  CHECK(NovaInput_CommandKey(KeyBindings::kSlotCount) == 0xffff);
  CHECK(NovaInput_CommandKey(0x9999) == 0xffff);
}
