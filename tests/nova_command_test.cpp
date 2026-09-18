#include <catch2/catch_test_macros.hpp>

#include <optional>

#include "nova_app.hpp"

// The main-menu command-token mapper (Ghidra 0x004872a0
// NovaCommand_DispatchToMode). The platform layer emits the raw character
// tokens; this locks in the original mapping, including the 'x'
// ACKNOWLEDGEMENTS branch, and that Escape is not aliased to quit.
TEST_CASE("main-menu command tokens map to their mode actions") {
  using enum GameModeAction;
  CHECK(NovaCommand_DispatchToMode('n') == std::optional{new_game});
  CHECK(NovaCommand_DispatchToMode('o') == std::optional{open_pilot});
  CHECK(NovaCommand_DispatchToMode('e') == std::optional{enter_spaceflight});
  CHECK(NovaCommand_DispatchToMode('p') == std::optional{preferences});
  CHECK(NovaCommand_DispatchToMode('a') == std::optional{about_nova});
  CHECK(NovaCommand_DispatchToMode('x') == std::optional{acknowledgements});
  CHECK(NovaCommand_DispatchToMode('q') == std::optional{quit});
}

TEST_CASE("unrecognised main-menu command tokens are ignored") {
  // Escape is the intro-skip/cancel slot (key binding 0x17), never a menu
  // command; the port must not treat it as quit.
  CHECK_FALSE(NovaCommand_DispatchToMode('\x1b').has_value());
  CHECK_FALSE(NovaCommand_DispatchToMode('m').has_value());
  CHECK_FALSE(NovaCommand_DispatchToMode('\\').has_value());
  CHECK_FALSE(NovaCommand_DispatchToMode('\0').has_value());
}
