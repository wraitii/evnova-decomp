#include "probe_state.hpp"

#include "game_state.hpp"
#include "nova_font.hpp"
#include "outfit.hpp"
#include "scenario_data.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace game {
namespace {

// Tiny JSON string builder (escaped strings, flat members).
struct Json {
  std::string text = "{";
  bool first_member = true;

  void key(std::string_view name) {
    if (!first_member) {
      text += ",";
    }
    first_member = false;
    text += "\"";
    text += name;
    text += "\":";
  }

  void str(std::string_view name, std::string_view value) {
    // JSON must be valid UTF-8; game strings are raw MacRoman.
    const std::string encoded = NovaText_EncodeUtf8(value);
    key(name);
    text += "\"";
    for (const char c : encoded) {
      switch (c) {
      case '"':
        text += "\\\"";
        break;
      case '\\':
        text += "\\\\";
        break;
      case '\n':
        text += "\\n";
        break;
      case '\r':
        text += "\\r";
        break;
      case '\t':
        text += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          text += ' ';
        } else {
          text += c;
        }
      }
    }
    text += "\"";
  }

  template <typename T> void num(std::string_view name, T value) {
    key(name);
    if constexpr (std::is_floating_point_v<T>) {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(value));
      text += buffer;
    } else {
      text += std::to_string(value);
    }
  }

  void boolean(std::string_view name, bool value) {
    key(name);
    text += value ? "true" : "false";
  }

  // Adds a pre-rendered array as one member (see JsonArr).
  void array(std::string_view name, const std::string &elements) {
    key(name);
    text += elements;
  }

  std::string done() {
    text += "}";
    return text;
  }
};

struct JsonArr {
  std::string text = "[";
  bool first = true;

  void raw(const std::string &element) {
    if (!first) {
      text += ",";
    }
    first = false;
    text += element;
  }

  std::string done() {
    text += "]";
    return text;
  }
};

[[nodiscard]] std::string StellarName(const GameState &state,
                                      std::int16_t stellar_id) {
  if (const auto *stellar = state.scenario.Stellar(stellar_id);
      stellar != nullptr) {
    return stellar->name;
  }
  return "";
}

} // namespace

std::string ProbeState_Snapshot(const GameState &state,
                                const std::string &query) {
  const std::int16_t system_id = state.player.current_system_id;
  const System *system =
      state.scenario.System(static_cast<std::int16_t>(system_id + 0x80));
  const ShipClass *ship_class = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));

  if (query.empty() || query == "summary") {
    Json j;
    j.boolean("game_active", state.game_active);
    j.str("pilot", state.pilot.first_name);
    j.num("credits", state.player.credits);
    j.num("system_id", system_id);
    j.str("system", system != nullptr ? system->name : "");
    j.num("ship_class_id", state.player.ship_class_id);
    j.str("ship_class", ship_class != nullptr ? ship_class->display_name : "");
    j.num("pos_x", state.player.pos_x);
    j.num("pos_y", state.player.pos_y);
    j.num("vel_x", state.player.vel_x);
    j.num("vel_y", state.player.vel_y);
    j.num("heading", state.player.heading);
    j.num("speed", state.player.speed);
    j.num("shield", state.player.shield_points);
    j.num("armor", state.player.armor_points);
    j.num("fuel", state.player.fuel_points);
    j.num("max_shield", state.cached_stats.max_shield_points);
    j.num("max_armor", state.cached_stats.max_armor_points);
    std::size_t active_missions = 0;
    for (const auto &flags : state.active_mission_runtime_flags) {
      if (flags.is_active) {
        ++active_missions;
      }
    }
    j.num("active_missions", active_missions);
    // Aggregate career score (Ghidra g_player_combat_rating_points). Exposed so
    // routes can gate on it, e.g. the pirate-hunt loop in
    // tests/scenarios/tutorial_to_combat_rating.toml.
    j.num("combat_rating", state.player_combat_rating_points);
    j.boolean("travel_engaging", state.travel.engaging);
    return j.done();
  }

  if (query == "player") {
    Json j;
    j.str("pilot", state.pilot.first_name);
    j.str("nickname", state.pilot.last_name);
    j.num("credits", state.player.credits);
    j.num("ship_class_id", state.player.ship_class_id);
    j.str("ship_name", state.player.ship_name);
    j.str("ship_class", ship_class != nullptr ? ship_class->display_name : "");
    j.num("pos_x", state.player.pos_x);
    j.num("pos_y", state.player.pos_y);
    j.num("vel_x", state.player.vel_x);
    j.num("vel_y", state.player.vel_y);
    j.num("heading", state.player.heading);
    j.num("speed", state.player.speed);
    j.num("shield", state.player.shield_points);
    j.num("armor", state.player.armor_points);
    j.num("fuel", state.player.fuel_points);
    j.num("max_shield", state.cached_stats.max_shield_points);
    j.num("max_armor", state.cached_stats.max_armor_points);
    j.num("fuel_capacity", state.cached_stats.fuel_capacity);
    j.num("primary_target_ship_slot", state.player.primary_target_ship_slot);
    j.num("combat_rating", state.player_combat_rating_points);
    j.num("current_system_id", system_id);
    return j.done();
  }

  if (query == "missions") {
    Json j;
    JsonArr rows;
    for (std::size_t slot = 0; slot < state.active_missions.size(); ++slot) {
      const auto &flags = state.active_mission_runtime_flags[slot];
      if (!flags.is_active) {
        continue;
      }
      const auto &mission = state.active_missions[slot];
      Json row;
      row.num("slot", slot);
      row.num("template_id", mission.mission_template_id);
      row.num("flags_primary", mission.flags_primary);
      row.num("flags_secondary", mission.flags_secondary);
      row.boolean("can_abort", mission.can_abort);
      row.boolean("failed", flags.is_failed);
      row.boolean("objective_complete", flags.objective_complete);
      row.boolean("carrying_resources", mission.carrying_resources);
      row.num("cargo_type_id", mission.cargo_type_id);
      row.num("cargo_qty_tons", mission.cargo_qty_tons);
      row.num("pickup_mode", mission.pickup_mode);
      row.num("drop_off_mode", mission.drop_off_mode);
      row.num("travel_stellar_id", mission.travel_stellar_id);
      // MisnActive stellar ids are 0-based indices; StellarName takes the
      // 0x80-based resource id.
      row.str("travel_stellar",
              StellarName(
                  state,
                  static_cast<std::int16_t>(mission.travel_stellar_id + 0x80)));
      row.num("return_stellar_id", mission.return_stellar_id);
      row.str("return_stellar",
              StellarName(
                  state,
                  static_cast<std::int16_t>(mission.return_stellar_id + 0x80)));
      row.num("reward", mission.resource_delta_or_cost);
      row.num("time_limit_days_remaining", mission.time_limit_days_remaining);
      row.num("goal_count_remaining", mission.goal_count_remaining);
      rows.raw(row.done());
    }
    j.array("missions", rows.done());
    return j.done();
  }

  if (query == "ships") {
    Json j;
    JsonArr rows;
    for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
      const Ship &ship = state.ShipAt(slot);
      if (!ship.is_active ||
          ship.current_system_id != state.player.current_system_id) {
        continue;
      }
      const ShipClass *cls = state.scenario.Ship(
          static_cast<std::int16_t>(ship.ship_class_id + 0x80));
      Json row;
      row.num("slot", slot);
      row.num("ship_class_id", ship.ship_class_id);
      row.str("ship_class", cls != nullptr ? cls->display_name : "");
      row.num("ship_instance_id", ship.ship_instance_id);
      row.num("government_id", ship.faction_or_government_id);
      // Mission/pers tagging and the capture gates. pers_def_slot is the
      // 0x80-based personality index; mission_fleet_slot == -1 marks an
      // ordinary ship (the boarding selector skips mission-fleet ships).
      row.num("pers_def_slot", ship.pers_def_slot);
      row.num("mission_fleet_slot", ship.mission_fleet_slot);
      // `squad_leader_ship_slot == 0` is the player's-attached test used by
      // Ship_CanPlayerHaveMoreEscorts (0x00468920); a non-escort has -1 and an
      // NPC escort carries its leader's slot, so the harness can separate the
      // player's fleet from system traffic.
      row.num("squad_leader_ship_slot", ship.squad_leader_ship_slot);
      row.num("boarded_target_latch", ship.boarded_target_latch);
      row.num("post_hit_mode_hint", ship.post_hit_mode_hint);
      // The values the capture-variant arbitration actually reads.
      row.num("class_default_ai_behavior",
              cls != nullptr ? cls->default_ai_behavior : -1);
      row.num("class_crew", cls != nullptr ? cls->crew : -1);
      row.num("pos_x", ship.pos_x);
      row.num("pos_y", ship.pos_y);
      row.num("vel_x", ship.vel_x);
      row.num("vel_y", ship.vel_y);
      row.num("heading", ship.heading);
      row.num("speed", ship.speed);
      row.num("shield", ship.shield_points);
      row.num("armor", ship.armor_points);
      // Raw tank plus the travel/jump locks, so a probe reader can tell a
      // fuel-starved ship from one held by its recorded destination or the
      // velocity-match gate in Stellar_CanShipInitiateJumpSequence.
      row.num("fuel_points", ship.fuel_points);
      row.num("jump_destination_stellar_id", ship.jump_destination_stellar_id);
      row.num("velocity_match_target_ship_slot",
              ship.velocity_match_target_ship_slot);
      row.num("ai_state_code", ship.ai_state_code);
      row.num("ai_behavior_code", ship.ai_behavior_code);
      row.num("ai_control_mode", ship.ai_control_mode);
      row.num("ai_desired_speed", ship.ai_desired_speed);
      row.num("ai_forward_thrust_cmd", ship.ai_forward_thrust_cmd);
      row.num("ai_station_hold_timer", ship.ai_station_hold_timer);
      row.num("ai_maneuver_timer", ship.ai_maneuver_timer_ms);
      row.num("primary_target_ship_slot", ship.primary_target_ship_slot);
      row.num("active_weapon_bank_slot", ship.active_weapon_bank_slot);
      row.num("ai_fire_trigger_latch", ship.ai_fire_trigger_latch);
      row.num("inbound_weapon_threat", ship.inbound_weapon_threat);
      if (ship.active_weapon_bank_slot >= 0 &&
          ship.active_weapon_bank_slot < 0x100) {
        const std::size_t bank =
            static_cast<std::size_t>(ship.active_weapon_bank_slot);
        row.num("active_weapon_ammo", ship.npc_weapon_count_by_class[bank]);
        row.num("active_weapon_secondary",
                ship.npc_weapon_secondary_count_by_class[bank]);
        row.num("active_weapon_cooldown", ship.npc_weapon_bank_cooldown[bank]);
      }
      row.boolean("arrival_monitor_active", ship.arrival_monitor_active);
      row.num("arrival_monitor_ticks", ship.arrival_monitor_elapsed_ticks);
      row.boolean("player_target",
                  state.player.primary_target_ship_slot ==
                      static_cast<std::int16_t>(slot));
      rows.raw(row.done());
    }
    j.array("ships", rows.done());
    return j.done();
  }

  if (query == "cargo") {
    Json j;
    j.num("credits", state.player.credits);
    // `capacity` is the player hull's own holds (cached outfit aggregate);
    // `fleet_capacity` adds eligible escort freighters' holds
    // (Player_ComputeFleetCargoCapacity 0x00469760), which is what the trade
    // center and HUD actually enforce.
    j.num("capacity", state.cached_stats.cargo_capacity);
    j.num("fleet_capacity", Player_ComputeFleetCargoCapacity(state));
    JsonArr bins;
    for (const auto tons : state.inventory.cargo_bins) {
      Json row;
      row.num("tons", tons);
      bins.raw(row.done());
    }
    j.array("bins", bins.done());
    JsonArr junk;
    for (std::size_t id = 0; id < state.inventory.junk_counts.size(); ++id) {
      const auto count = state.inventory.junk_counts[id];
      if (count == 0) {
        continue;
      }
      Json row;
      row.num("id", id);
      row.num("count", count);
      junk.raw(row.done());
    }
    j.array("junk", junk.done());
    return j.done();
  }

  if (query == "travel") {
    Json j;
    j.num("travel_slot", state.travel.travel_slot);
    j.boolean("engaging", state.travel.engaging);
    j.num("engaged_stellar_id", state.travel.engaged_stellar_id);
    j.num("destination_system_id", state.travel.destination_system_id);
    j.num("starmap_destination_system_id",
          state.travel.starmap_destination_system_id);
    j.num("selected_stellar_id", state.travel.selected_stellar_id);
    j.boolean("selected_stellar_is_manual",
              state.travel.selected_stellar_is_manual);
    j.boolean("hyperspace_mode", state.travel.hyperspace_mode);
    return j.done();
  }

  if (query == "system") {
    Json j;
    j.num("id", system_id);
    j.str("name", system != nullptr ? system->name : "");
    j.num("government_id", system != nullptr ? system->government_id : -1);
    j.num("avg_ships", system != nullptr ? system->avg_ships : 0);
    if (system_id >= 0 &&
        system_id < static_cast<std::int16_t>(state.system_reputation.size())) {
      j.num("reputation", state.system_reputation[system_id]);
    }
    return j.done();
  }

  Json j;
  j.str("error", "unknown query");
  j.str("query", query);
  return j.done();
}

} // namespace game
