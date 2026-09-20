#include <catch2/catch_test_macros.hpp>

#include "util/format.hpp"

using evnova::util::UpperFirstAscii;

// MWRuntime_ToUpper (Ghidra 0x004d6260) is the MetroWerks C-locale toupper:
// only 'a'..'z' change and bytes >= 0x80 are passed through. Several original
// UI paths capitalize the first character of a label or count word with it.
TEST_CASE(
    "UpperFirstAscii capitalizes only a leading ASCII lower-case letter") {
  REQUIRE(UpperFirstAscii("credits") == "Credits");
  REQUIRE(UpperFirstAscii("one") == "One");
  REQUIRE(UpperFirstAscii("Credits") == "Credits");
  REQUIRE(UpperFirstAscii("") == "");
  REQUIRE(UpperFirstAscii("3 tons") == "3 tons");
  // A non-ASCII lead byte must not be folded (the runtime table is identity
  // above 0x7f and the caller passes a signed char).
  REQUIRE(UpperFirstAscii("\xC3\xA9lan") == "\xC3\xA9lan");
}
