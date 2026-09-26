#include <catch2/catch_test_macros.hpp>

#include "game/extended_prefs.hpp"

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

using game::NovaExtraPrefs;
using game::PresentationScale_Parse;

TEST_CASE("presentation scale parser accepts the supported range only") {
  REQUIRE(PresentationScale_Parse("0.5") == 0.5F);
  REQUIRE(PresentationScale_Parse("1") == 1.0F);
  REQUIRE(PresentationScale_Parse("1.0") == 1.0F);
  REQUIRE(PresentationScale_Parse("2.5") == 2.5F);
  REQUIRE(PresentationScale_Parse("4") == 4.0F);

  // Complete-value parse: trailing junk, whitespace, signs, non-finite and
  // out-of-range values are all rejected.
  REQUIRE_FALSE(PresentationScale_Parse("").has_value());
  REQUIRE_FALSE(PresentationScale_Parse("0").has_value());
  REQUIRE_FALSE(PresentationScale_Parse("-1").has_value());
  REQUIRE_FALSE(PresentationScale_Parse("4.5").has_value());
  REQUIRE_FALSE(PresentationScale_Parse("0.4").has_value());
  REQUIRE_FALSE(PresentationScale_Parse("1.0x").has_value());
  REQUIRE_FALSE(PresentationScale_Parse(" 2.5").has_value());
  REQUIRE_FALSE(PresentationScale_Parse("abc").has_value());
  REQUIRE_FALSE(PresentationScale_Parse("nan").has_value());
  REQUIRE_FALSE(PresentationScale_Parse("inf").has_value());
}

TEST_CASE("extra prefs parse the display section and fall back on bad values") {
  std::istringstream in("# comment\n"
                        "[paths]\n"
                        "install_root=/games/EV Nova\n"
                        "\n"
                        "[display]\n"
                        "ui_scale=2\n"
                        "flight_scene_scale=1.5\n"
                        "mission_scale=bogus\n"
                        "unknown_key=7\n");
  NovaExtraPrefs prefs;
  game::NovaExtraPrefs_Parse(in, prefs, "<test>");
  REQUIRE(prefs.install_root.has_value());
  REQUIRE(prefs.install_root->generic_string() == "/games/EV Nova");
  REQUIRE(prefs.scale.ui == 2.0F);
  REQUIRE(prefs.scale.flight_scene == 1.5F);
  REQUIRE(prefs.scale.mission == 1.0F); // invalid -> fallback
}

TEST_CASE("extra prefs write and reparse every field") {
  NovaExtraPrefs prefs;
  prefs.install_root = std::filesystem::path{"/tmp/EV Nova"};
  prefs.scale = {1.25F, 2.0F, 0.75F};

  std::ostringstream out;
  game::NovaExtraPrefs_Write(out, prefs);

  std::istringstream in(out.str());
  NovaExtraPrefs parsed;
  game::NovaExtraPrefs_Parse(in, parsed, "<round-trip>");
  REQUIRE(parsed.install_root == prefs.install_root);
  REQUIRE(parsed.scale.ui == 1.25F);
  REQUIRE(parsed.scale.flight_scene == 2.0F);
  REQUIRE(parsed.scale.mission == 0.75F);
}

TEST_CASE("extra prefs write omits an unset install root") {
  NovaExtraPrefs prefs;
  prefs.install_root.reset();

  std::ostringstream out;
  game::NovaExtraPrefs_Write(out, prefs);

  std::istringstream in(out.str());
  NovaExtraPrefs parsed;
  game::NovaExtraPrefs_Parse(in, parsed, "<round-trip>");
  REQUIRE_FALSE(parsed.install_root.has_value());
  REQUIRE(parsed.scale.ui == 1.0F);
  REQUIRE(parsed.scale.flight_scene == 1.0F);
  REQUIRE(parsed.scale.mission == 1.0F);
}

TEST_CASE("mission dialogs compose ui and mission on top of each other") {
  using game::PresentationScale;
  const PresentationScale neutral{};
  REQUIRE(neutral.mission_dialog() == 1.0F);
  const PresentationScale scaled{1.5F, 2.0F, 1.25F};
  // Single composed request; the fit clamp is applied by the placement builder.
  REQUIRE(scaled.mission_dialog() == 1.875F);
  // Mission dialogs are authored UI, so the flight factor never enters.
  const PresentationScale flight_only{1.0F, 3.0F, 1.0F};
  REQUIRE(flight_only.mission_dialog() == 1.0F);
}

TEST_CASE("presentation scale resolution honours loaded values without env") {
  if (std::getenv("EVN_UI_SCALE") != nullptr ||
      std::getenv("EVN_FLIGHT_SCENE_SCALE") != nullptr ||
      std::getenv("EVN_MISSION_SCALE") != nullptr) {
    SKIP("environment override set; resolution test skipped");
  }
  NovaExtraPrefs prefs;
  prefs.scale = {1.25F, 2.0F, 0.75F};
  const auto resolved = game::NovaExtraPrefs_ResolvePresentationScale(prefs);
  REQUIRE(resolved.ui == 1.25F);
  REQUIRE(resolved.flight_scene == 2.0F);
  REQUIRE(resolved.mission == 0.75F);
}
