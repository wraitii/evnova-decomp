#include "escort_commands.hpp"

#include "hud_overlay.hpp"
#include "log.hpp"
#include "nova_random.hpp"
#include "ship_ai.hpp"
#include "spaceflight.hpp"

#include <algorithm>

namespace game {
namespace {

// Ui_ShowTargetCategoryPanel 0x0049e8d0 sets the panel fade level to 0x20.
// It remains fully visible until 0x1e0 ticks after the last interaction, then
// the level counts down once per tick (0x00450c13 / 0x00452990).
constexpr std::int16_t kPanelShowTicks = 0x20;
constexpr std::int64_t kPanelInactivityTicks = 0x1e0;

// Ghidra ShipState +0x88 ai_behavior_code value for a carried ship deployed
// by Weapon_SpawnShipFromCarrierBayWeapon 0x0041e640.
constexpr std::int16_t kDeployedFighterBehavior = 5;

// Sums over the attached-ship cohort (active, escorting the player). The
// auto-cancel arms (0x00450e48/0x00450ee1) additionally require
// escort_command_pending (ShipState +0xC4).
enum class AttachFilter {
  kAny,         // active + squad_leader_ship_slot == 0
  kJumpCapable, // + not disabled, not destroyed (the count cache)
  kRecoverable, // + behavior 5 (deployed fighters) + escort_command_pending
  kOrdered,     // + escort_command_pending
};

[[nodiscard]] int CountAttached(const GameState &state,
                                AttachFilter filter,
                                std::int16_t category) {
  int count = 0;
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    const Ship &ship = state.ShipAt(slot);
    if (!ship.is_active || ship.squad_leader_ship_slot != 0) {
      continue;
    }
    if (filter == AttachFilter::kJumpCapable &&
        (NovaAiShip_IsDisabled(state, ship) || NovaAiShip_IsDestroyed(ship))) {
      continue;
    }
    if (filter == AttachFilter::kRecoverable &&
        (ship.ai_behavior_code != kDeployedFighterBehavior ||
         ship.escort_command_pending == 0)) {
      continue;
    }
    if (filter == AttachFilter::kOrdered && ship.escort_command_pending == 0) {
      continue;
    }
    if (category >= 0) {
      const ShipClass *cls = state.scenario.Ship(
          static_cast<std::int16_t>(ship.ship_class_id + 0x80));
      if (cls == nullptr || cls->class_category != category) {
        continue;
      }
    }
    ++count;
  }
  return count;
}

[[nodiscard]] bool GroupPresent(const GameState &state, std::int16_t category) {
  return CountAttached(state, AttachFilter::kAny, category) > 0;
}

// @port 0x0049e8d0 80% ui
// SDL defers rendering to HudRenderer::DrawEscortCommandsPanel during the HUD
// refresh.
// Ghidra 0x0049E8D0 Ui_ShowTargetCategoryPanel.
// SDL defers the original's immediate draw to the next HUD refresh.
void ShowEscortPanel(EscortCommandState &escort) {
  escort.panel_timer = kPanelShowTicks;
}

void QueueTransitionSound(GameState &state, std::int16_t index) {
  state.pending_ui_sounds.push_back({index, 1});
}

// The order-feedback phrase after a dispatch (0x0045cbxx tail): the "-ing"
// forms while the player's hyperspace jump timer has expired, the "will ..."
// forms otherwise; Attack splits on whether the player has a valid target.
[[nodiscard]] std::string OrderPhrase(const GameState &state,
                                      std::int16_t command) {
  const bool ready = state.player.hyperspace_jump_timer <= 0.0F;
  std::uint16_t entry = 0;
  if (ready) {
    switch (command) {
    case 3:
      entry = 0x9b;
      break; // "returning to hangar."
    case 0:
      entry = 0x9c;
      break; // "returning to formation."
    case 4:
      entry = 0x9d;
      break; // "holding position."
    case 1:
      entry = 0x9e;
      break; // "defending."
    default:
      entry = 0x9a;
      break; // "will attack." / "attacking target."
    }
  } else {
    switch (command) {
    case 3:
      entry = 0x96;
      break; // "will return to hangar."
    case 0:
      entry = 0x97;
      break; // "will return to formation."
    case 4:
      entry = 0x98;
      break; // "will hold position."
    case 1:
      entry = 0x99;
      break; // "will defend."
    default:
      entry = 0x9a;
      break; // "will attack."
    }
  }
  if (command == 2) {
    // Original split: with a valid primary target that is not itself an
    // attached ship, the immediate form reads "attacking target." (0x9f);
    // otherwise the plain "will attack." (0x9a).
    const std::int16_t player_target = state.player.primary_target_ship_slot;
    if (ready && player_target != -1 &&
        state.SlotInRange(static_cast<std::size_t>(player_target)) &&
        state.ShipAt(static_cast<std::size_t>(player_target))
                .squad_leader_ship_slot != 0) {
      entry = 0x9f; // "attacking target."
    }
  }
  const auto text = NovaHud_LoadStringEntry(0x7d2, entry);
  return text.value_or("");
}

} // namespace

// @port 0x00450710 45% gameplay,ui,license,synthetic
// Ghidra Ship_HandlePlayerShipCore 0x0044AA70 auxiliary escort-command CFGs:
// panel entry 0x00450B4E, selection/order arms 0x00450C24..0x00450F67, with
// reordered continuations at 0x00452820..0x004529A8.
void PlayerTick_EscortCommands(GameState &state,
                               const EscortCommandInput &input,
                               std::int64_t now_60hz) {
  EscortCommandState &escort = state.escort;

  // ---- Panel toggle (0x00450ae7, arms 0x00452820 / 0x0045293b / 0x004529a8)
  if (input.panel_toggle_held) {
    if (escort.panel_toggle_latch == 0) {
      escort.panel_toggle_latch = 1;
      if (escort.panel_timer > 0) {
        // Toggle closed (0x004529a8 via local_265).
        escort.close_pending = true;
      } else if (CountAttached(state, AttachFilter::kAny, -1) == 0) {
        // 0x00452820: no attached ships -> STR# 0x7d2 0x33 + denied cue.
        if (auto text = NovaHud_LoadStringEntry(0x7d2, 0x33)) {
          NovaHud_ShowOverlayMessage(state, *text, std::uint64_t{0xfa});
        }
        QueueTransitionSound(state, 3);
      } else {
        // 0x0045293b: open on All Ships. A still-showing "no escorts"
        // message is dismissed. Provisional: the original's CString_Equals
        // (0x0046ef00) only compares the first byte; the port compares the
        // whole text.
        QueueTransitionSound(state, 0);
        escort.selected_category = -1;
        const auto no_escorts = NovaHud_LoadStringEntry(0x7d2, 0x33);
        if (state.hud_overlay.active && no_escorts.has_value() &&
            state.hud_overlay.message == *no_escorts) {
          NovaHud_ClearOverlayMessage(state);
        }
        ShowEscortPanel(escort);
        escort.key_time_60hz = now_60hz;
      }
    }
  } else {
    escort.panel_toggle_latch = 0;
  }

  if (escort.panel_timer <= 0) {
    // 0x00452a44: cache reset while the panel is closed. The selection keys
    // are not polled (their latches hold their last value).
    escort.attached_count_cache = -1;
    escort.close_pending = false;
  } else {
    // ---- Attached-count cache + inactivity fade (0x00450b95..0x00450c19).
    // While the panel is open the filtered attached count is re-derived every
    // frame; a change refreshes the cache (an empty cohort deselects and
    // zeroes the interaction stamp). Independently, 0x1e0 ticks after the
    // last interaction the timer decays once per tick; reaching 0 closes.
    const int count = CountAttached(state, AttachFilter::kJumpCapable, -1);
    if (count != escort.attached_count_cache) {
      if (count == 0) {
        escort.key_time_60hz = 0;
        escort.selected_category = -1;
      }
      escort.attached_count_cache = static_cast<std::int16_t>(count);
    }
    if (now_60hz > escort.key_time_60hz + kPanelInactivityTicks) {
      --escort.panel_timer;
      if (escort.panel_timer == 0) {
        escort.close_pending = true;
      }
    }
    if (escort.close_pending) {
      // 0x004529a8: close; the open/close cue plays while the toggle key is
      // held (the toggle itself, not the dismiss key or the fade).
      escort.close_pending = false;
      escort.panel_timer = 0;
      if (input.panel_toggle_held) {
        QueueTransitionSound(state, 0);
      }
    } else {
      // ---- Selection rows (0x00450c24..0x00450cd4, arm 0x004529f7). Row
      // 0 is All Ships; rows 1..4 select a class-category group only when one
      // of its ships is attached. Every fresh press stamps the interaction
      // time, even when the group is absent.
      for (int row = 0; row < 5; ++row) {
        if (!input.select_group_held[row]) {
          escort.select_key_latch[row] = 0;
          continue;
        }
        if (escort.select_key_latch[row] != 0) {
          continue;
        }
        escort.select_key_latch[row] = 1;
        const std::int16_t category = static_cast<std::int16_t>(row - 1);
        if (row == 0 || GroupPresent(state, category)) {
          escort.selected_category = category;
          ShowEscortPanel(escort);
          QueueTransitionSound(state, 2);
        }
        escort.key_time_60hz = now_60hz;
      }
    }
  }

  // ---- Order keys (0x00450cf4..0x00450dcc, far arms 0x00452a52..0x00452af5).
  const std::array<bool, 4> order_held{input.order_attack_held,
                                       input.order_defend_held,
                                       input.order_hold_held,
                                       input.order_formation_held};
  for (int key = 0; key < 4; ++key) {
    if (!order_held[key]) {
      escort.order_key_latch[key] = 0;
      continue;
    }
    if (escort.order_key_latch[key] != 0) {
      continue;
    }
    escort.order_key_latch[key] = 1;
    // 0x00452a52/62/72/82: Attack / Defend / Hold Position / Formation, with
    // the arm-modifier pair turning Formation into Return to Hangar
    // (0x00450d52).
    std::int16_t command = 0;
    switch (key) {
    case 0:
      command = 2;
      break;
    case 1:
      command = 1;
      break;
    case 2:
      command = 4;
      break;
    default:
      command = input.arm_modifier_held ? 3 : 0;
      break;
    }
    if (escort.selected_category == -1 || escort.panel_timer <= 0) {
      // 0x00452aa2: All Ships or a closed panel arms every category.
      state.target_category_command.fill(command);
    } else {
      state.target_category_command[escort.selected_category] = command;
    }
    if (escort.panel_timer > 0) {
      if (Ship_CommandPlayerEscortGroup(
              state, escort.selected_category, command, false)) {
        ShowEscortPanel(escort);
        escort.key_time_60hz = now_60hz;
      }
    } else {
      // 0x00452ae0: panel closed -> the order always targets every ship.
      Ship_CommandPlayerEscortGroup(state, -1, command, false);
    }
  }

  // ---- Auto-cancel arms (0x00450ddb..0x00450f67). A pending Return to
  // Hangar with no deployed fighter of that category still carrying an
  // order (escort_command_pending), or any order for a category with no
  // attached ship carrying one, reverts to Formation and refreshes the panel.
  for (std::int16_t category = 0; category < 4; ++category) {
    if (state.target_category_command[category] == 3 &&
        CountAttached(state, AttachFilter::kRecoverable, category) == 0) {
      state.target_category_command[category] = 0;
      if (escort.panel_timer > 0) {
        ShowEscortPanel(escort);
      }
    }
  }
  for (std::int16_t category = 0; category < 4; ++category) {
    if (CountAttached(state, AttachFilter::kOrdered, category) == 0 &&
        state.target_category_command[category] != 0) {
      state.target_category_command[category] = 0;
      if (escort.panel_timer > 0) {
        ShowEscortPanel(escort);
      }
    }
  }
}

[[nodiscard]] bool Player_HasEscortGroup(const GameState &state,
                                         std::int16_t category) {
  return GroupPresent(state, category);
}

// @port 0x0045C880 100%
// Ghidra 0x0045c880 Ship_CommandPlayerEscortGroup.
bool Ship_CommandPlayerEscortGroup(GameState &state,
                                   std::int16_t category,
                                   std::int16_t command,
                                   bool suppress_message) {
  bool accepted = false;
  // Any accepted order change came from a class without the mute flag
  // (flags_secondary 0x10); gates the combat-chatter acknowledgement.
  bool voiced_change = false;
  // Chatter voice: the first eligible ship seeds it (government < -1 is the
  // unset sentinel), later ones replace it on a 1-in-2 roll.
  std::int16_t chatter_government = -2;
  std::int16_t chatter_variant = -1;
  std::int16_t message_command = command;
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &ship = state.ShipAt(slot);
    if (!ship.is_active || ship.squad_leader_ship_slot != 0) {
      continue;
    }
    const ShipClass *cls = state.scenario.Ship(
        static_cast<std::int16_t>(ship.ship_class_id + 0x80));
    if (cls == nullptr || (category != -1 && cls->class_category != category)) {
      continue;
    }
    const bool voiced = (cls->flags_secondary & 0x10U) == 0U;
    if (command != ship.escort_command_code) {
      ship.escort_command_pending = 1;
      if (ship.ai_behavior_code == kDeployedFighterBehavior) {
        ship.escort_command_code = command;
        accepted = true;
        voiced_change = voiced_change || voiced;
      } else if (command == 3) {
        // Non-deployed ships cannot return to a hangar: the order reverts
        // them to Formation (and the message phrase follows).
        if (ship.escort_command_code != 0) {
          ship.escort_command_code = 0;
          message_command = 0;
          accepted = true;
          voiced_change = voiced_change || voiced;
        }
      } else {
        ship.escort_command_code = command;
        accepted = true;
        voiced_change = voiced_change || voiced;
      }
    }
    // Attack propagates the player's primary target to the group, but only
    // when that target is not itself a player-squad ship
    // (Ship_IsInPlayerSquad 0x0046b8d0, called at 0x0045c963). The clean-room
    // keeps a slot-range guard so the lookup stays in bounds.
    if (ship.escort_command_code == 2) {
      const std::int16_t player_target = state.player.primary_target_ship_slot;
      if (player_target != -1 &&
          player_target != ship.primary_target_ship_slot &&
          state.SlotInRange(static_cast<std::size_t>(player_target)) &&
          !NovaShip_IsInPlayerSquad(
              state, state.ShipAt(static_cast<std::size_t>(player_target)))) {
        ship.primary_target_ship_slot = player_target;
        accepted = true;
      }
    }
    if (command != 3 && NovaAiShip_IsShipInAiBehavior5State5(ship)) {
      ship.primary_target_ship_slot = -1;
      ship.ai_secondary_target_slot = -1;
      if (chatter_government < -1 || RandomBelow(state, 2) == 0) {
        chatter_variant = ship.voice_type_mode;
        chatter_government = cls->inherent_attributes_govt;
      }
      accepted = true;
    }
    // The original evaluates this voice roll for every matching ship once any
    // ship (this or an earlier one) has accepted.
    if (accepted && voiced &&
        (chatter_government < -1 || RandomBelow(state, 2) == 0)) {
      chatter_government = cls->inherent_attributes_govt;
      chatter_variant = ship.voice_type_mode;
    }
  }
  if (!accepted) {
    return false;
  }
  if (suppress_message) {
    return true;
  }

  // "New escort orders assigned:  <group> <order>" (STR# 0x7d2 0x86 + group
  // 0x87..0x8b + phrase 0x96..0x9f), 0xfa ticks.
  std::string message;
  if (auto text = NovaHud_LoadStringEntry(0x7d2, 0x86)) {
    message = *text;
  }
  const std::uint16_t group_entry = category == 0   ? 0x87
                                    : category == 1 ? 0x88
                                    : category == 2 ? 0x89
                                    : category == 3 ? 0x8a
                                                    : 0x8b;
  if (auto text = NovaHud_LoadStringEntry(0x7d2, group_entry)) {
    message += *text;
  }
  message += " ";
  message += OrderPhrase(state, message_command);
  NovaHud_ShowOverlayMessage(state, message, std::uint64_t{0xfa});
  // Acknowledgement cue: g_transition_sound_handle_table[1] (DAT_00591564).
  QueueTransitionSound(state, 1);
  if (!voiced_change) {
    return true;
  }
  // Frame_QueueCombatChatter kind 1 when the attack order names a non-squad
  // target (squad_leader_ship_slot != 0), else kind 0.
  std::int16_t chatter_kind = 0;
  if (message_command == 2) {
    const std::int16_t player_target = state.player.primary_target_ship_slot;
    if (player_target != -1 &&
        state.SlotInRange(static_cast<std::size_t>(player_target)) &&
        state.ShipAt(static_cast<std::size_t>(player_target))
                .squad_leader_ship_slot != 0) {
      chatter_kind = 1;
    }
  }
  NovaFrame_QueueCombatChatter(state,
                               chatter_kind,
                               std::max<std::int16_t>(chatter_government, -1),
                               chatter_variant);
  return true;
}

} // namespace game
