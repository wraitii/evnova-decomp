#pragma once

namespace game {

// Runtime policy for clean-room fixes to confirmed bugs in the original
// executable or shipped scenario data. Every use is marked `BUGFIX(original)`
// so a fix can always be traced back to the defect it corrects.
//
// The policy is loaded from the port-only `EV Nova Extra Prefs.ini`
// (`[bugfixes]`) at startup and is owned per-session by GameState. The Extra
// Prefs dialog edits it live; the default (all flags true) reproduces the
// previous all-on behavior.
//
// `safe` bundles the fixes that correct clear defects without changing the
// game's intended balance or connectivity. The remaining flags
// each cover a correction that shifts observable play, so a player wanting a
// more faithful original experience can turn them off individually.
struct BugFixPolicy {
  // Display, mission, cloak, weapon and respawn correctness fixes. Recommended
  // on; see docs/known_original_bugs.md for the bundled list.
  bool safe = true;
  // ModType 45/46 max-gun/turret bonuses scale with the owned count.
  bool outfit_slot_balance = true;
  // Outfits charge the allied-rank scaled price the shipyard already uses.
  bool outfit_prices = true;
  // Crön event odds, date windows and holdoff lifecycle follow the Bible.
  bool cron_events = true;
  // System murk fades SWParticle weapon sparks and debris.
  bool particle_fog = true;
  // Weapon reload and beam Count both run on the 30 Hz design clock: carried
  // cooldown lets a bank fire several shots per call, so every mount adds its
  // share of the fire rate (known bugs 62 and 92). Off reproduces the
  // original's once-per-call fire cap and raw-call beam lifetime.
  bool weapon_cadence = true;
};

} // namespace game
