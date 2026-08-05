#include "pilot_file.hpp"

#include "../log.hpp"

namespace game {

PilotFile PilotFile::Fresh() {
  // Ghidra 0x004cd4b0 PilotData_InitializePlayerState, absent-block seed:
  // g_ship_states->credits = 10000, ship_class_id = 0, current_system_id = 0,
  // combat rating = 0, in-game date 1999/1/1, per-govt reputation reset. The
  // date/reputation/combat-rating fields are not tracked by GameState yet, so
  // only the ship-core defaults are seeded here.
  PilotFile fresh;
  fresh.credits = 10000;
  fresh.ship_class_id = 0;
  fresh.current_system_id = 0;
  return fresh;
}

void PilotFileApply(const PilotFile &pilot_file, GameState &state) {
  // The original copies the seeded/loaded pilot-save block into the live
  // globals (PilotData_InitializePlayerState fresh-seed and IntroCinematic_
  // SetupFrames). Apply the tracked subset into GameState.
  state.pilot.first_name = pilot_file.pilot_name;

  state.player.credits = pilot_file.credits;
  state.player.ship_class_id = pilot_file.ship_class_id;
  state.player.current_system_id = pilot_file.current_system_id;
  state.player.active_weapon_bank_slot = pilot_file.active_weapon_bank_slot;
  state.player.timed_action_counter = pilot_file.timed_action_counter;
  state.player.death_timer_active = pilot_file.death_timer_active;
  state.player.shield_points = pilot_file.shield_points;
  state.player.armor_points = pilot_file.armor_points;
  state.player.fuel_points = pilot_file.fuel_points;
  state.player.pos_x = pilot_file.pos_x;
  state.player.pos_y = pilot_file.pos_y;
  state.player.vel_x = pilot_file.vel_x;
  state.player.vel_y = pilot_file.vel_y;
  state.player.heading = pilot_file.heading;
  state.player.speed = pilot_file.speed;

  state.intro_cinematic.source_pict_ids = pilot_file.intro_source_pict_ids;
  state.intro_cinematic.duration_60h_ticks =
      pilot_file.intro_duration_60h_ticks;
  state.intro_cinematic.post_intro_dest_id = pilot_file.post_intro_dest_id;

  state.outfit_owned_count = pilot_file.outfit_owned_count;
  state.weapon_bank_ammo = pilot_file.weapon_bank_ammo;
  state.weapon_bank_secondary = pilot_file.weapon_bank_secondary;

  // The pilot record is held in memory only: the reimplementation does not
  // write .plt files, so saving is intentionally not performed here.
  NovaLog::Debug("pilot file '{}' applied to live state (in-memory; not "
                 "persisted to a .plt)",
                 pilot_file.pilot_name);
}

} // namespace game
