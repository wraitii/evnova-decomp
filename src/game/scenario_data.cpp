#include "scenario_data.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "ship_visual.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string_view>

namespace game {
namespace {

[[nodiscard]] std::uint16_t ReadBe16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  if (offset + 2 > bytes.size()) {
    return 0;
  }
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(bytes[offset]) << 8U |
      std::to_integer<std::uint8_t>(bytes[offset + 1]));
}

[[nodiscard]] std::int16_t ReadBeI16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  return static_cast<std::int16_t>(ReadBe16(bytes, offset));
}

[[nodiscard]] std::uint32_t ReadBe32(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  if (offset + 4 > bytes.size()) {
    return 0;
  }
  return static_cast<std::uint32_t>(
      std::to_integer<std::uint8_t>(bytes[offset]) << 24U |
      std::to_integer<std::uint8_t>(bytes[offset + 1]) << 16U |
      std::to_integer<std::uint8_t>(bytes[offset + 2]) << 8U |
      std::to_integer<std::uint8_t>(bytes[offset + 3]));
}

[[nodiscard]] std::int32_t ReadBeI32(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  return static_cast<std::int32_t>(ReadBe32(bytes, offset));
}

// NUL-terminated C string at `offset` (bounded by the payload). Returns an
// empty string when missing.
[[nodiscard]] std::string ReadCString(std::span<const std::byte> bytes,
                                      std::size_t offset) {
  if (offset >= bytes.size()) {
    return {};
  }
  const auto *begin = reinterpret_cast<const char *>(bytes.data() + offset);
  const auto max_len = bytes.size() - offset;
  const void *nul = std::memchr(begin, '\0', max_len);
  const std::size_t len =
      nul == nullptr
          ? max_len
          : static_cast<std::size_t>(static_cast<const char *>(nul) - begin);
  return std::string{begin, len};
}

// NUL-terminated C string at `offset`, additionally bounded to `max_len`
// bytes (fixed-width string fields).
[[nodiscard]] std::string ReadCStringBounded(std::span<const std::byte> bytes,
                                             std::size_t offset,
                                             std::size_t max_len) {
  return ReadCString(bytes.first(std::min(bytes.size(), offset + max_len)),
                     offset);
}

// NameString_StripSubtitleSuffix (0x004cd230): truncates a display name at
// the first ';' and trims trailing spaces ("Base Name;Sub " -> "Base Name").
[[nodiscard]] std::string StripSubtitleSuffix(std::string_view name) {
  if (const auto cut = name.find(';'); cut != std::string_view::npos) {
    name = name.substr(0, cut);
  }
  while (!name.empty() && name.back() == ' ') {
    name.remove_suffix(1);
  }
  return std::string{name};
}

[[nodiscard]] MissionDef DecodeMission(std::span<const std::byte> bytes) {
  MissionDef mission;
  const auto copy_size = std::min(bytes.size(), mission.raw_payload.size());
  std::copy_n(bytes.begin(), copy_size, mission.raw_payload.begin());

  // These leading fields are the resource values copied into MisnDef by
  // NovaResources_LoadMisnResourceDefs (0x0043bbb0). The old decoder treated
  // the return id and fail/success locators as if they started two bytes
  // later, which shifted the mission target-resolution inputs.
  mission.link_system_filter = ReadBeI16(bytes, 0x00);
  // Ghidra NovaResources_LoadMisnResourceDefs (0x0043bbb0) copies these
  // fields from the loader's native offsets. Bible names (availability block
  // + the two travel locators): AvailLoc/AvailRecord/AvailRating/AvailRandom,
  // then TravelStel/ReturnStel. Payload +0x02 is skipped by the loader.
  mission.avail_location = ReadBeI16(bytes, 0x04);
  mission.avail_record = ReadBeI16(bytes, 0x06);
  mission.avail_rating = ReadBeI16(bytes, 0x08);
  mission.avail_random = ReadBeI16(bytes, 0x0a);
  mission.travel_stellar_locator = ReadBeI16(bytes, 0x0c);
  mission.return_stellar_locator = ReadBeI16(bytes, 0x0e);
  mission.cargo_qty_tons = ReadBeI16(bytes, 0x12);
  mission.cargo_type_resource = ReadBeI16(bytes, 0x10);
  mission.ship_restriction_filter = ReadBeI16(bytes, 0x5a);

  // Mission_PopulateMissionSlotFromDef (0x0043f8c0) reads the mission-ship
  // dude/system group directly from +0x20..+0x2c. The separate +0x52 dude
  // value belongs to the loader's auxiliary definition projection and is not
  // the active mission-ship dude used by the population path.
  // The accepted-mission population path reads the active ship dude from
  // resource +0x24. The loader also projects a separate validation copy from
  // +0x52, which is not the field used by Mission_PopulateMissionSlotFromDef.
  mission.special_ship_dude = ReadBeI16(bytes, 0x24);
  mission.aux_ship_system = ReadBeI16(bytes, 0x22);
  mission.aux_ship_dude = ReadBeI16(bytes, 0x24);
  // Payload +0x40 is the mission TimeLimit (days); the accepted-mission
  // population copies it into MisnActive +0x45.
  mission.time_limit_days = ReadBeI16(bytes, 0x40);
  mission.pickup_mode = ReadBeI16(bytes, 0x14);
  mission.drop_off_mode = ReadBeI16(bytes, 0x16);
  mission.scan_mask = ReadBeI16(bytes, 0x18);
  mission.on_resolve_repeat_count = ReadBeI16(bytes, 0x48);
  // PayVal: populate (0x0043f8c0) reads this 4-byte field from payload +0x1c
  // into MisnActive +0x22. The prior +0x4a read overlapped AuxShipDude/
  // AuxShipSyst and fed garbage into the fee/pay chain.
  mission.resource_delta_or_cost = ReadBeI32(bytes, 0x1c);
  mission.aux_ships_left = ReadBeI16(bytes, 0x50);
  mission.initial_ship_count = ReadBeI16(bytes, 0x52);
  // The active-slot population copies these two values from resource +0x50
  // and +0x52 into MisnActive +0x55/+0x57.
  mission.flags_primary = ReadBe16(bytes, 0x50);
  mission.flags_secondary = ReadBe16(bytes, 0x52);
  mission.target_ship_count = ReadBeI16(bytes, 0x20);
  mission.current_system_locator = ReadBeI16(bytes, 0x22);
  mission.special_ship_name_string_id = ReadBeI16(bytes, 0x2a);
  mission.spawn_behavior = ReadBeI16(bytes, 0x26);
  mission.fleet_spawn_goal = ReadBeI16(bytes, 0x28);
  mission.random_text_string_id = ReadBeI16(bytes, 0x32);
  mission.special_ship_spawn_mode = ReadBeI16(bytes, 0x2c);
  mission.competing_government_id = ReadBeI16(bytes, 0x2e);
  mission.competing_reputation_delta = ReadBeI16(bytes, 0x30);
  mission.mission_ship_count_max = ReadBeI16(bytes, 0x48);
  // Bible aux-ship fields: AuxShipDude (+0x4a) and AuxShipSyst (+0x4c); the
  // prior reads were swapped relative to populate (0x0043f8c0).
  mission.mission_fleet_metric = ReadBeI16(bytes, 0x4c);
  mission.auxiliary_ship_dude = ReadBeI16(bytes, 0x4a);
  mission.can_abort = ReadBeI16(bytes, 0x42) != 0;
  for (std::size_t i = 0; i < mission.text_description_ids.size(); ++i) {
    mission.text_description_ids[i] =
        ReadBeI16(bytes, 0x34 + i * sizeof(std::int16_t));
  }
  mission.ship_done_text_id = ReadBeI16(bytes, 0x44);
  mission.slot_aux_text_id = ReadBeI16(bytes, 0x58);
  mission.initial_briefing_id = mission.text_description_ids.front();
  mission.availability_expr = ReadCString(bytes, 0x5c);
  // 64-bit Require mask (Outfit_EvaluateRequireMask gate in the offering
  // eligibility chain, 0x00441b40).
  mission.require_mask_lo = ReadBe32(bytes, 0x656);
  mission.require_mask_hi = ReadBe32(bytes, 0x65a);
  mission.list_priority = ReadBeI16(bytes, 0x7a0);
  // NovaResources_LoadMisnResourceDefs (0x0043bbb0) canonicalizes these
  // sentinels before the BBS eligibility pass: negative link filters mean
  // "any source", while a negative return stellar falls back to stellar 0.
  if (mission.link_system_filter < -1) {
    mission.link_system_filter = -1;
  }
  if (mission.avail_location < 0) {
    mission.avail_location = 0;
  }
  return mission;
}

// ---------------------------------------------------------------------------
// p\x91rs (PersDef / personality) decode
// ---------------------------------------------------------------------------
// Ground truth is the personality pass of NovaData_LoadScenarioResourceTables
// (0x004bd3c0): each record is a fixed big-endian layout (see PersDef
// comments for the def-side offsets) with per-field validation replicated
// below. The loader guarantees a 0x190-byte payload before reading the tail
// fields; records shorter than the field window decode to an inactive def.
[[nodiscard]] PersDef DecodePers(std::span<const std::byte> bytes,
                                 const std::vector<ShipClass> &ship_table) {
  PersDef def;
  if (bytes.size() < 0x180) {
    return def;
  }
  def.present = true;
  def.loaded_latch = true;

  def.spawn_system_filter = ReadBeI16(bytes, 0x00);
  def.government_id = ReadBeI16(bytes, 0x02);
  // Govt is stored +0x80; anything outside 0x80..0x17f means independent.
  if (def.government_id < 0x80 || 0x17f < def.government_id) {
    def.government_id = -1;
  } else {
    def.government_id = static_cast<std::int16_t>(def.government_id - 0x80);
  }

  def.ai_behavior_code = ReadBeI16(bytes, 0x04);
  def.aggression_level = ReadBeI16(bytes, 0x06);
  def.cowardice_pct = ReadBeI16(bytes, 0x08);

  // ShipType: ids outside 0x80..0x37f decode to class 0 (original quirk —
  // the loader's else-branch writes 0, not the -1 sentinel); an in-range id
  // naming a nonexistent class (tech_level -9999) decodes to -1.
  def.ship_class_id = ReadBeI16(bytes, 0x0a);
  if (def.ship_class_id < 0x80 || 0x37f < def.ship_class_id) {
    def.ship_class_id = 0;
  } else {
    def.ship_class_id = static_cast<std::int16_t>(def.ship_class_id - 0x80);
    if (def.ship_class_id < static_cast<std::int16_t>(ship_table.size()) &&
        ship_table[static_cast<std::size_t>(def.ship_class_id)].tech_level ==
            kShipClassNonexistentTechLevel) {
      def.ship_class_id = -1;
    }
  }

  // Four weapon triples: WeapType (+0x0c..), WeapCount (+0x14..), AmmoLoad
  // (+0x1c..). The Bible documents eight slots but the binary reads and
  // applies only four, spread into 0x100-entry tables indexed by weapon id
  // minus 0x80. Ids below 0x80 (including the -1/0 "none" values) add
  // nothing; out-of-table ids would overwrite adjacent memory in the
  // original — here they are skipped (no shipped record uses one).
  for (std::size_t slot = 0; slot < 4; ++slot) {
    const std::int16_t weapon_id = ReadBeI16(bytes, 0x0c + slot * 2);
    if (weapon_id <= 0x7f) {
      continue;
    }
    const auto index = static_cast<std::size_t>(weapon_id) - 0x80;
    if (index >= def.weapon_count_delta.size()) {
      continue;
    }
    def.weapon_count_delta[index] = ReadBeI16(bytes, 0x14 + slot * 2);
    def.weapon_ammo_load_delta[index] = ReadBeI16(bytes, 0x1c + slot * 2);
  }

  def.booty_base_credits = ReadBeI32(bytes, 0x24);
  // ShieldMod percent; the loader divides by the 100.0 double at 0x00575e48
  // at decode time. Negative values mean "invincible" downstream.
  def.shield_armor_scale = static_cast<float>(ReadBeI16(bytes, 0x28)) / 100.0F;

  def.hail_pict_id = ReadBeI16(bytes, 0x2a);
  if (def.hail_pict_id < 0x80) {
    def.hail_pict_id = -1;
  }
  def.comm_quote_id = ReadBeI16(bytes, 0x2c);
  def.hail_quote_id = ReadBeI16(bytes, 0x2e);
  // LinkMission is stored +0x80 (mission resource id), no upper clamp.
  def.link_mission_id = ReadBeI16(bytes, 0x30);
  if (def.link_mission_id < 0x80) {
    def.link_mission_id = -1;
  } else {
    def.link_mission_id = static_cast<std::int16_t>(def.link_mission_id - 0x80);
  }
  def.flags_primary = ReadBe16(bytes, 0x32);
  def.flags_secondary = ReadBe16(bytes, 0x17e);

  // ActiveOn occupies payload +0x34..+0x133 (0x100-byte field).
  def.availability_expression = ReadCStringBounded(bytes, 0x34, 0x100);

  // Boarded-grant triple. The loader reads +0x134 -> def+0x784 (class,
  // < 1 invalidates the whole grant), +0x136 -> def+0x788, +0x138 ->
  // def+0x786, then clamps def+0x786 to 0..100 and def+0x788 to >= 0. The
  // clamp windows make def+0x786 the probability and def+0x788 the count,
  // which is the reverse of the Bible's GrantProb/GrantCount listing order.
  def.grant_item_class = ReadBeI16(bytes, 0x134);
  def.grant_count = ReadBeI16(bytes, 0x136);
  def.grant_probability = ReadBeI16(bytes, 0x138);
  if (def.grant_item_class < 1) {
    def.grant_item_class = -1;
    def.grant_count = 0;
    def.grant_probability = 0;
  } else {
    if (def.grant_probability < 0) {
      def.grant_probability = 0;
    }
    if (def.grant_probability > 100) {
      def.grant_probability = 100;
    }
    if (def.grant_count < 0) {
      def.grant_count = 0;
    }
  }

  // Payload +0x13a free-text name (loader bounds the region to +0x179 and
  // converts it to a Pascal string in place).
  def.special_ship_name = ReadCStringBounded(bytes, 0x13a, 0x40);

  // Colour word at +0x17a squashed to three 5-bit channels exactly as the
  // loader shifts them (>>19, >>5, >>3 — the non-uniform shifts are
  // original); field naming is provisional.
  const std::uint32_t color = ReadBe32(bytes, 0x17a);
  def.color_r5 = static_cast<std::uint8_t>((color >> 19U) & 0x1fU);
  def.color_g5 = static_cast<std::uint8_t>((color >> 5U) & 0x1fU);
  def.color_b5 = static_cast<std::uint8_t>((color >> 3U) & 0x1fU);
  return def;
}

// ---------------------------------------------------------------------------
// w\x91ap (Weapon) decode
// ---------------------------------------------------------------------------
// Offsets verified against the Nova Data 4 payloads and the original loader
// (0x004bd3c0), which copies each resource +N into a g_weapon_defs slot. The
// first packet follows Bible order (Reload0, Count2, MassDmg4, EnergyDmg6,
// Guidance8, Speed_a, AmmoType_c, Graphic_e, Inaccuracy10, Sound12, Impact14,
// ExplodType16, ProxRadius18, BlastRadius1a, Flags1c, Seeker1e) and the
// remaining fields match the loader's WeaponDef mapping (flags_secondary +0x48,
// burst_cycle_ticks +0x5a / burst_reset_cooldown +0x5c, turret arc +0x30,
// homing/turn +0x32, kickback +0x56, turret_group +0x58, retarget +0x68,
// jam_vuln +0x5e..+0x64). See the Weapon field comments in scenario_data.hpp.
[[nodiscard]] Weapon DecodeWeapon(std::span<const std::byte> bytes) {
  Weapon w;
  // Offsets cross-checked against the in-memory loader copy (0x004bd3c0):
  // each `resource +N` below is the payload offset the loader stores into a
  // g_weapon_defs slot. See the Weapon field comments in scenario_data.hpp
  // for the Ghidra/provisional names.
  w.reload_ticks = ReadBeI16(bytes, 0x00); // Reload / WeaponDef speed_scalar
  w.lifetime_ticks = ReadBeI16(bytes, 0x02);
  w.mass_damage = ReadBeI16(bytes, 0x04);
  w.energy_damage = ReadBeI16(bytes, 0x06);
  w.guidance_mode = ReadBeI16(bytes, 0x08);
  w.weapon_mode_code = w.guidance_mode;
  w.projectile_speed = static_cast<float>(ReadBeI16(bytes, 0x0a));
  w.ammo_type = ReadBeI16(bytes, 0x0c);
  w.sprite_id = ReadBeI16(bytes, 0x0e);
  w.inaccuracy = ReadBeI16(bytes, 0x10);
  w.fire_sound = ReadBeI16(bytes, 0x12);
  w.impact_impulse = ReadBeI16(bytes, 0x14);
  w.impact_effect_id = ReadBeI16(bytes, 0x16);
  w.blast_radius = ReadBeI16(bytes, 0x18);
  w.splash_radius = ReadBeI16(bytes, 0x1a);
  w.fuse_ticks = ReadBeI16(bytes, 0x22);
  w.late_collision_window_ticks = ReadBeI16(bytes, 0x46);
  w.ionization_points = ReadBeI16(bytes, 0x4a);
  w.ionization_color = ReadBe32(bytes, 0x72) & 0x00ffffffU;
  w.flags = ReadBe16(bytes, 0x1c); // flags_primary
  w.flags_quaternary = ReadBe16(bytes, 0x1e);
  w.flags_secondary = ReadBe16(bytes, 0x48);
  w.flags_tertiary = ReadBe16(bytes, 0x66);
  w.beam_length_px = ReadBeI16(bytes, 0x30);
  w.shot_anim_frame_dwell = ReadBeI16(bytes, 0x32);
  // Bible MaxAmmo (payload +0x6c -> WeaponDef +0x1e, loader line 0x004c4bxx):
  // per-instance ammo/bay capacity, 0/-1 = defer to the outfit Max field.
  w.max_ammo = ReadBeI16(bytes, 0x6c);
  // Beam render fields (Bible Falloff/BeamColor/CoronaColor/LiDensity/
  // LiAmplitude), with the loader's post-read normalization from 0x004bd3c0:
  // beams without an explicit falloff default to 0x10; lightning beams clamp
  // density to >= 2, drop the corona falloff, and force width/amplitude >= 1.
  w.beam_falloff = ReadBeI16(bytes, 0x34);
  w.beam_core_color = ReadBe32(bytes, 0x36) & 0x00ffffffU;
  w.beam_corona_color = ReadBe32(bytes, 0x3a) & 0x00ffffffU;
  w.beam_lightning_density = ReadBeI16(bytes, 0x6e);
  w.beam_lightning_amplitude = ReadBeI16(bytes, 0x70);
  if (w.beam_length_px > 0 && w.beam_falloff < 1) {
    w.beam_falloff = 0x10;
  }
  if (w.beam_lightning_density > 0) {
    if (w.beam_lightning_density < 2) {
      w.beam_lightning_density = 2;
    }
    w.beam_falloff = 0;
    if (w.shot_anim_frame_dwell < 1) {
      w.shot_anim_frame_dwell = 1;
    }
    if (w.beam_lightning_amplitude < 1) {
      w.beam_lightning_amplitude = 1;
    }
  }
  // GuidedTurn (payload +0x6a): loader scales by 0.1 into the float
  // WeaponDef.guided_turn_rate (degrees/tick used by shot guidance).
  w.guided_turn_rate = static_cast<float>(ReadBeI16(bytes, 0x6a)) * 0.1F;
  w.kickback_impulse = ReadBeI16(bytes, 0x56);
  w.turret_group_id = ReadBeI16(bytes, 0x58);
  w.burst_cycle_ticks = ReadBeI16(bytes, 0x5a);
  w.burst_reset_cooldown = ReadBeI16(bytes, 0x5c);
  w.retarget_interval_ticks = ReadBeI16(bytes, 0x68);
  w.range_link_gate = ReadBeI16(bytes, 0x3e);
  const auto range_link_resource_id = ReadBeI16(bytes, 0x40);
  w.range_link_weapon_id =
      range_link_resource_id >= 0x80
          ? static_cast<std::int16_t>(range_link_resource_id - 0x80)
          : -1;
  w.range_link_extra_count = ReadBeI16(bytes, 0x44);
  for (std::size_t i = 0; i < 4; ++i) {
    w.jam_vuln[i] = ReadBeI16(bytes, 0x5e + i * 2);
  }
  return w;
}

void ComputeWeaponEffectiveRanges(std::vector<Weapon> &weapons) {
  // NovaData_LoadScenarioResourceTables (0x004bd3c0) performs this pass
  // after loading every weapon. Speed is stored in the resource as pixels per
  // frame * 100, while the runtime range field uses normalized pixels/frame.
  for (std::size_t index = 0; index < weapons.size(); ++index) {
    Weapon &weapon = weapons[index];
    weapon.range_scalar = 0.0F;
    const auto mode = weapon.weapon_mode_code;
    if (mode == 0 || mode == 3 || mode == 10) {
      weapon.range_scalar = static_cast<float>(weapon.beam_length_px);
      continue;
    }
    const bool projectile_range_mode =
        mode == -1 || mode == 1 || mode == 4 || (mode >= 6 && mode <= 9);
    if (!projectile_range_mode) {
      continue;
    }

    std::size_t current = index;
    for (std::size_t hops = 0; hops <= weapons.size(); ++hops) {
      Weapon &linked = weapons[current];
      weapon.range_scalar += static_cast<float>(linked.lifetime_ticks) *
                             (linked.projectile_speed / 100.0F);
      if (linked.range_link_gate < 1 || linked.range_link_weapon_id < 0 ||
          linked.range_link_weapon_id >=
              static_cast<std::int16_t>(weapons.size())) {
        break;
      }
      const auto next = static_cast<std::size_t>(linked.range_link_weapon_id);
      if (next == current) {
        if (linked.range_link_extra_count > 0) {
          weapon.range_scalar +=
              static_cast<float>(linked.lifetime_ticks) *
              static_cast<float>(linked.range_link_extra_count) *
              (linked.projectile_speed / 100.0F);
        }
        break;
      }
      current = next;
    }
  }
}

// ---------------------------------------------------------------------------
// sh\x95p (ShipClass) decode
// ---------------------------------------------------------------------------
// All offsets verified against the Nova Data 1 payloads and the loader's ship
// section. Bible order with the octal-pack quirk that the 8 stock weapon banks
// are stored as 2 groups of 4: banks 0-3 as (type,count,ammo) triples at
// 0x12/0x1a/0x22 and banks 4-7 at 0x6ce/0x6d6/0x6de. The default items are
// likewise split: ids 0-3 at 0x4e (counts 0x56) and ids 4-7 at 0x370 (counts
// 0x378).
[[nodiscard]] ShipClass DecodeShip(std::span<const std::byte> bytes) {
  ShipClass s;
  s.cargo_holds = ReadBeI16(bytes, 0x00); // Holds
  s.base_shield = ReadBeI16(bytes, 0x02);
  s.accel = static_cast<float>(ReadBeI16(bytes, 0x04));
  s.speed = static_cast<float>(ReadBeI16(bytes, 0x06));
  s.turn_rate = static_cast<float>(ReadBeI16(bytes, 0x08));
  s.base_fuel = ReadBeI16(bytes, 0x0a);
  s.free_mass = ReadBeI16(bytes, 0x0c);
  s.base_armor = ReadBeI16(bytes, 0x0e);
  // Bible ShieldRech/ArmorRech units are shield/armor points * 1000 per
  // reference frame: 1000 means one point per 30 Hz frame.
  s.shield_recharge = static_cast<float>(ReadBeI16(bytes, 0x10)) / 1000.0F;

  for (std::size_t i = 0; i < 4; ++i) {
    s.stock_weapons[i].weapon_id = ReadBeI16(bytes, 0x12 + i * 2);
    s.stock_weapons[i].count = ReadBeI16(bytes, 0x1a + i * 2);
    s.stock_weapons[i].ammo_load = ReadBeI16(bytes, 0x22 + i * 2);
  }
  if (bytes.size() >= 0x6de + 8 * 2) {
    for (std::size_t i = 0; i < 4; ++i) {
      s.stock_weapons[i + 4].weapon_id = ReadBeI16(bytes, 0x6ce + i * 2);
      s.stock_weapons[i + 4].count = ReadBeI16(bytes, 0x6d6 + i * 2);
      s.stock_weapons[i + 4].ammo_load = ReadBeI16(bytes, 0x6de + i * 2);
    }
  }

  s.max_gun = ReadBeI16(bytes, 0x2a);
  s.max_turret = ReadBeI16(bytes, 0x2c);
  s.tech_level = ReadBeI16(bytes, 0x2e);
  s.cost = ReadBeI32(bytes, 0x30);
  s.death_delay_frames = ReadBeI16(bytes, 0x34);
  s.armor_recharge = static_cast<float>(ReadBeI16(bytes, 0x36)) / 1000.0F;
  s.destruction_effect_while_breaking = ReadBeI16(bytes, 0x38);
  s.destruction_effect_final = ReadBeI16(bytes, 0x3a);
  s.display_weight = ReadBeI16(bytes, 0x3c);
  s.mass_tons = ReadBeI16(bytes, 0x3e);
  s.length_meters = ReadBeI16(bytes, 0x40);
  s.default_ai_behavior = ReadBeI16(bytes, 0x42);
  s.timed_action_counter_init = ReadBeI16(bytes, 0x4c);
  s.crew = ReadBeI16(bytes, 0x44); // == Ghidra ShipClassDef.capture_power
  // The loader clamps negatives to 0 (0x004bd3c0): a negative crew count
  // would otherwise read as boardable/capturing.
  if (s.crew < 0) {
    s.crew = 0;
  }
  s.strength = ReadBeI16(bytes, 0x46);
  s.inherent_combat_govt = ReadBeI16(bytes, 0x48);
  s.inherent_attributes_govt = s.inherent_combat_govt;
  s.capability_flags = ReadBe16(bytes, 0x4a);

  for (std::size_t i = 0; i < 4; ++i) {
    s.default_outfit_ids[i] = ReadBeI16(bytes, 0x4e + i * 2);
    s.default_outfit_counts[i] = ReadBeI16(bytes, 0x56 + i * 2);
  }
  if (bytes.size() >= 0x376 + 2) {
    for (std::size_t i = 0; i < 4; ++i) {
      s.default_outfit_ids[i + 4] = ReadBeI16(bytes, 0x370 + i * 2);
      s.default_outfit_counts[i + 4] = ReadBeI16(bytes, 0x378 + i * 2);
    }
  }
  s.skill_variance_percent = ReadBeI16(bytes, 0x60);
  // Bible FuelRegen (payload +0x5e -> ShipClassDef +0x34): frames per 1 unit
  // of fuel regenerated; verified against loader 0x004bd3c0 (payload 0x5e
  // feeds the short consumed by Ship_ComputeShipFuelRechargeRate 0x00463b30).
  s.fuel_regen = ReadBeI16(bytes, 0x5e);
  // Ghidra NovaData_LoadScenarioResourceTables (0x004bd3c0) copies the
  // ionization capacity from ShipClassDef.ionization_capacity at payload
  // +0x36c. It is distinct from the nearby default-outfit count block.
  s.ionization_capacity = ReadBeI16(bytes, 0x36c);
  // Bible KeyCarried (payload +0x36e -> ShipClassDef +0xa04). Loader
  // 0x004c1967..0x004c19be: raw BE copy, then values >= 0x80 are rebased to
  // the zero-based class id; anything below stores -1 (unset). Runs even
  // when ionization capacity clamps to 0 (LAB_004c42b0 jumps back in).
  if (bytes.size() >= 0x370) {
    const std::int16_t key = ReadBeI16(bytes, 0x36e);
    s.key_carried_ship_class = key >= 0x80
                                   ? static_cast<std::int16_t>(key - 0x80)
                                   : static_cast<std::int16_t>(-1);
  }
  s.ionization_decay_rate =
      std::max(1.0F, static_cast<float>(ReadBeI16(bytes, 0x36a)) * 0.01F);
  s.flags_secondary = ReadBe16(bytes, 0x62);
  if (bytes.size() >= 0x728) {
    s.availability_flags = ReadBe16(bytes, 0x726);
  }
  // The string block and store masks are copied verbatim by
  // NovaData_LoadScenarioResourceTables (0x004bd3c0).  Only the scripts used
  // by landed stores are named here; the two intervening script blocks remain
  // intentionally unmodelled pending callsite attribution.
  // Ghidra 0x004bd3c0 ship section: ShipClassDef +0xac <- payload +0x6e6 C
  // string (Bible Subtitle), Pascal-ized in memory and drawn on the target
  // display under the ship name.
  if (bytes.size() >= 0x6e7) {
    s.subtitle = ReadCString(bytes, 0x6e6);
  }
  s.availability_expr = ReadCString(bytes, 0x6c);
  s.on_purchase_expr = ReadCString(bytes, 0x26a);
  s.on_retire_expr = ReadCString(bytes, 0x4cf);
  s.short_name = ReadCString(bytes, 0x5ce);
  s.long_name = ReadCString(bytes, 0x62e);
  s.buy_random = ReadBeI16(bytes, 0x388);
  if (s.buy_random > 100) {
    s.buy_random = 100;
  }
  s.hire_random = ReadBeI16(bytes, 0x38a);
  if (s.hire_random > 100) {
    s.hire_random = 100;
  } // Store masks (loader 0x004bd3c0): Contribute at shp +0x64/+0x68 and
    // Require
  // at +0x380/+0x384 feed the player's aggregate Contribute/Require masks.
  // (These are the ship-class baseline contributions combined with owned
  // outfits for Require checks; the earlier payload at +0x72a is the ship's
  // cost, not a mask.)
  s.contribute_lo = ReadBe32(bytes, 0x64);
  s.contribute_hi = ReadBe32(bytes, 0x68);
  if (bytes.size() >= 0x384 + 4) {
    s.require_lo = ReadBe32(bytes, 0x380);
    s.require_hi = ReadBe32(bytes, 0x384);
  }
  return s;
}

// ---------------------------------------------------------------------------
// o\x9ftf (Outfit) decode
// ---------------------------------------------------------------------------
// Offsets verified against the o\x9ftf payloads and the loader's outfit
// section (0x004bd3c0). The record *name* is supplied separately from the
// BRGR resource.map record name (never a numeric payload field). Payload
// layout:
//   +0x00 Cost low word / ordering value, +0x02 Mass, +0x04 TechLevel,
//   +0x06/+0x08 ModType1/modVal1, +0x0a Max, +0x0c Flags, +0x0e Cost (32-bit),
//   +0x12..+0x1c ModType2-4/ModVal2-4, +0x1e/+0x22 Contribute (32-bit pair),
//   +0x26/+0x2a Require (32-bit pair), +0x2e Availability, +0x12d OnPurchase,
//   +0x32b ShortName, +0x36b LCName, +0x3ab LCPlural, +0x3ec DispWeight,
//   +0x3ee Graphic sprite id, +0x3f0 BuyRandom, +0x3f2 ItemClass.
[[nodiscard]] Outfit DecodeOutfit(std::span<const std::byte> bytes) {
  Outfit o;
  o.mass_tons = ReadBeI16(bytes, 0x02);           // Mass
  o.tech_level = ReadBeI16(bytes, 0x04);          // TechLevel
  o.mod_type = ReadBeI16(bytes, 0x06);            // ModType (primary)
  o.mod_val = ReadBeI16(bytes, 0x08);             // ModVal (primary)
  o.max_count = ReadBeI16(bytes, 0x0a);           // Max
  o.flags = ReadBe16(bytes, 0x0c);                // Flags
  o.cost = ReadBeI32(bytes, 0x0e);                // Cost (4 bytes, big-endian)
  o.alt_mod_types[0] = ReadBeI16(bytes, 0x12);    // ModType2
  o.alt_mod_vals[0] = ReadBeI16(bytes, 0x14);     // ModVal2
  o.alt_mod_types[1] = ReadBeI16(bytes, 0x16);    // ModType3
  o.alt_mod_vals[1] = ReadBeI16(bytes, 0x18);     // ModVal3
  o.alt_mod_types[2] = ReadBeI16(bytes, 0x1a);    // ModType4
  o.alt_mod_vals[2] = ReadBeI16(bytes, 0x1c);     // ModVal4
  o.contribute_lo = ReadBe32(bytes, 0x1e);        // Contribute (low 32)
  o.contribute_hi = ReadBe32(bytes, 0x22);        // Contribute (high 32)
  o.require_lo = ReadBe32(bytes, 0x26);           // Require (low 32)
  o.require_hi = ReadBe32(bytes, 0x2a);           // Require (high 32)
  o.availability_expr = ReadCString(bytes, 0x2e); // Availability
  o.on_purchase_expr = ReadCString(bytes, 0x12d); // OnPurchase
  o.on_sell_expr = ReadCString(bytes, 0x22c);     // OnSell
  o.short_name = ReadCString(bytes, 0x32b);       // ShortName
  o.lc_name = ReadCString(bytes, 0x36b);          // LCName
  o.lc_plural = ReadCString(bytes, 0x3ab);        // LCPlural
  o.display_weight = ReadBeI16(bytes, 0x3ec);     // DispWeight
  o.sprite_id = ReadBeI16(bytes, 0x3ee);          // Graphic (p\x9ari sprite)
  o.stock_threshold = ReadBeI16(bytes, 0x3f0);    // In-Stock % (clamped 0..100)
  if (o.stock_threshold < 0) {
    o.stock_threshold = 0;
  }
  if (o.stock_threshold > 100) {
    o.stock_threshold = 100;
  }
  o.item_class = ReadBeI16(bytes, 0x3f2); // ItemClass
  o.persistent_on_ship_swap = (o.flags & 0x0004U) != 0U;
  // The loader (0x004bd3c0) stores weapon/ammo/bomb mod values zero-based,
  // subtracting 0x80 from any ModVal > 0x7f when the matching ModType is 1
  // (kWeapon), 3 (kAmmo) or 0x15 (kBomb). This makes a weapon outfit's mod_val
  // equal the zero-based weapon bank slot (resource id minus 0x80), so weapon
  // banking (NovaWeapon_*) indexes banks directly with mod_val.
  const auto rebase_mod_val = [](std::int16_t mod_type, std::int16_t &mod_val) {
    if ((mod_type == 1 || mod_type == 3 || mod_type == 0x15) &&
        mod_val > 0x7f) {
      mod_val = static_cast<std::int16_t>(mod_val - 0x80);
    }
  };
  rebase_mod_val(o.mod_type, o.mod_val);
  for (std::size_t i = 0; i < o.alt_mod_types.size(); ++i) {
    rebase_mod_val(o.alt_mod_types[i], o.alt_mod_vals[i]);
  }
  return o;
}

// ---------------------------------------------------------------------------
// sp\x9ab (Stellar / sp\xf6b) decode
// ---------------------------------------------------------------------------
// Offsets verified against Nova Data 2's stellar payloads and the loader's
// stellar section (0x004bd3c0): xPos+0, yPos+2, link_a_id+4 (the primary spin
// sprite-set id, id+1000 is the sp\x9an resource), travel_flags (32-bit)+6,
// TechLevel+0x0c, SpecialTech1-3+0x0e and SpecialTech4-8+0x444,
// reputation_threshold+0x16, availability_flags+0x20, service_cost+0x234,
// link_b_id+0x240. government_id+0x14 is rebased into the 0.. space and set
// to -1 when < 0x80.
[[nodiscard]] Stellar DecodeStellar(std::span<const std::byte> bytes) {
  Stellar st;
  st.pos_x = ReadBeI16(bytes, 0x00);     // xPos
  st.pos_y = ReadBeI16(bytes, 0x02);     // yPos
  st.link_a_id = ReadBeI16(bytes, 0x04); // link_a_id (primary spin set)
  if (st.link_a_id < 0 || st.link_a_id > 0xff) {
    st.link_a_id = -1;
  }
  st.flags = ReadBe32(bytes, 0x06);       // travel_flags
  st.tech_level = ReadBeI16(bytes, 0x0c); // TechLevel
  st.special_tech[0] = ReadBeI16(bytes, 0x0e);
  st.special_tech[1] = ReadBeI16(bytes, 0x10);
  st.special_tech[2] = ReadBeI16(bytes, 0x12);
  st.government_id = ReadBeI16(bytes, 0x14); // Govt (resource id)
  // Loader 0x004bd3c0 rebases into the 0.. space: < 0x80 -> -1, else -0x80.
  // The AvailStel govt lanes (m\xefsn 10000..31999) compare these rebased ids.
  if (st.government_id < 0x80) {
    st.government_id = -1;
  } else {
    st.government_id = static_cast<std::int16_t>(st.government_id - 0x80);
  }
  st.min_status = ReadBeI16(bytes, 0x16);             // reputation_threshold
  st.engage_highlight_frame = ReadBeI16(bytes, 0x18); // hypergate pulse frame
  st.cust_snd_id = ReadBeI16(bytes, 0x1a);            // CustSndID
  st.availability_flags = ReadBe16(bytes, 0x20);      // availability_flags
  if ((st.availability_flags & 0x3000U) != 0U) {
    // CustSndID is an emergence angle only for hypergates (0x1000) and
    // wormholes (0x2000); ordinary stellar sound ids remain untouched.
    st.emergence_angle_deg = st.cust_snd_id;
  }
  // Animation timing (Bible AnimDelay / Frame0Bias; Ghidra StellarDef +0x470/
  // +0x472 from payload +0x22/+0x24). See Stellar_UpdateStellarSprites.
  if (bytes.size() >= 0x26) {
    st.animation_dwell_time = ReadBeI16(bytes, 0x22);
    st.animation_frame_multiplier = ReadBeI16(bytes, 0x24);
  }
  if (bytes.size() >= 0x242) {
    st.link_b_id = ReadBeI16(bytes, 0x240); // link_b_id (alternate spin set)
    if (st.link_b_id < 0 || st.link_b_id > 0xff) {
      st.link_b_id = -1;
    }
  } else {
    st.link_b_id = -1;
  }
  if (bytes.size() >= 0x44e) {
    for (std::size_t i = 0; i < 5; ++i) {
      st.special_tech[i + 3] = ReadBeI16(bytes, 0x444 + i * 2);
    }
  }
  // service_cost (payload +0x234; StellarDef +0x38), a destination-service
  // value whose collection path is not yet reconstructed.
  if (bytes.size() >= 0x238) {
    st.service_cost = ReadBeI32(bytes, 0x234);
  }
  // Payload +0x238 (StellarDef +0x46c) -> the starmap "gravity shear"
  // hazard flag (NovaUi_RedrawStarmapWindow 0x004a62f0).
  if (bytes.size() >= 0x23a) {
    st.gravity_shear = ReadBeI16(bytes, 0x238);
  }
  // Tribute (StellarDef +0x46a <- payload +0x0a; the loader falls back to
  // 1000 x TechLevel for < 1, applied by the daily income pass 0x00423540).
  st.tribute = ReadBeI16(bytes, 0x0a);
  if (st.tribute < 1) {
    st.tribute = static_cast<std::int16_t>(st.tech_level * 1000);
  }
  // Schedule/garrison tail (loader 0x004bd3c0): garrison size (payload
  // +0x1e -> StellarDef +0x468) seeds the present-ship count with the
  // loader's rescale branches; the ambient sprite population (+0x23c ->
  // +0x40, the daily tick's activity gate) and the schedule countdown seed
  // (+0x242 -> +0x47a) + fire script (+0x345 -> +0x365) drive the per-day
  // schedule slice of Mission_TickDailyWorldUpdate.
  if (bytes.size() >= 0x240) {
    st.max_ship_count = ReadBeI16(bytes, 0x1e);
    if (st.max_ship_count < 0x3e9) {
      st.present_ship_count = st.max_ship_count;
    } else if (st.max_ship_count < 0x2711) {
      st.present_ship_count = st.max_ship_count / 10 - 100;
    } else {
      st.present_ship_count = st.max_ship_count / 10 - 1000;
    }
    st.sprite_population = ReadBeI32(bytes, 0x23c);
    st.sprite_handle_active = st.sprite_population < 0;
  }
  if (bytes.size() >= 0x346) {
    st.schedule_days = ReadBeI16(bytes, 0x242);
  }
  if (bytes.size() >= 0x445) {
    st.schedule_script = ReadCStringBounded(bytes, 0x345, 0xff);
  }
  return st;
}

// crön decode (loader cron pass, NovaData_LoadScenarioResourceTables
// 0x004bd3c0). The three strings are NUL-terminated C strings inside fixed
// 255-byte payload fields (the loader CString_Copy's them verbatim).
[[nodiscard]] CronEventDef DecodeCron(std::span<const std::byte> bytes) {
  CronEventDef def;
  def.present = true;
  def.first_day = ReadBeI16(bytes, 0x00);
  def.first_month = ReadBeI16(bytes, 0x02);
  def.first_year = ReadBeI16(bytes, 0x04);
  def.last_day = ReadBeI16(bytes, 0x06);
  def.last_month = ReadBeI16(bytes, 0x08);
  def.last_year = ReadBeI16(bytes, 0x0a);
  def.trigger_odds = ReadBeI16(bytes, 0x0c);
  def.duration = ReadBeI16(bytes, 0x0e);
  def.pre_holdoff = ReadBeI16(bytes, 0x10);
  def.post_holdoff = ReadBeI16(bytes, 0x12);
  def.flags = ReadBe16(bytes, 0x16);
  def.enable_on = ReadCStringBounded(bytes, 0x18, 0xff);
  def.on_start = ReadCStringBounded(bytes, 0x117, 0xff);
  def.on_end = ReadCStringBounded(bytes, 0x216, 0xff);
  def.contribute_lo = ReadBe32(bytes, 0x316);
  def.contribute_hi = ReadBe32(bytes, 0x31a);
  def.require_lo = ReadBe32(bytes, 0x31e);
  def.require_hi = ReadBe32(bytes, 0x322);
  for (std::size_t i = 0; i < 4; ++i) {
    std::int16_t govt = ReadBeI16(bytes, 0x326 + i * 2);
    // Loader: < 0x80 -> no government, else rebase to the 0.. space.
    def.news_govts[i] =
        govt < 0x80 ? -1 : static_cast<std::int16_t>(govt - 0x80);
    std::int16_t str = ReadBeI16(bytes, 0x32e + i * 2);
    // Loader: < -1 -> no STR# id.
    def.govt_news_strs[i] = str < -1 ? -1 : str;
  }
  return def;
}

// ---------------------------------------------------------------------------
// g\x9avt (Government / govmnt) decode
// ---------------------------------------------------------------------------
// Field offsets verified against Nova Data 1's government payloads and the
// loader's government section (0x004bd3c0; loop reads the payload big-endian
// and derives voice_type_mode, the fly-scaled skill fractions and the 8-bit
// theme colors). Payload layout (payload offsets in parentheses):
//   voice_type_code +0, flags_primary +2, scan_mask_short +4, jam1 +6,
//   flee +8, disable_penalty +10, board +12, kill +14, shoot +16, max_odds
//   +18, bribe +20, combat_rating_src +22, class1-4 +0x18, ally1-4 +0x20,
//   enemy1-4 +0x28, pilot_skill_src +0x30, ai_skill +0x32, comm_name +0x34,
//   target_code +0x44, scan_lo +0x54, scan_hi +0x58, jam2-4 +0x5c..+0x62,
//   medium_name +0x64, theme_color RGB24 +0xa4, ship_color RGB24 +0xa8,
//   interface_id +0xac, news_pic_id +0xae. The record name (resource.map, via
//   ResourceData_ReadEntryMetadata + StripSubtitleSuffix) is the display name.
// Name-table mapping verified against the loader's government pass: the
// runtime 0x100-stride pstring tables are g_government_name_table <- +0x44
// (the Bible's TargetCode, drawn by the target-status panel),
// g_government_comm_name_table <- +0x34 (CommName) and
// g_government_medium_name_table <- +0x64 (MediumName).
[[nodiscard]] Government DecodeGovernment(std::span<const std::byte> bytes) {
  Government g;

  g.voice_type_code = ReadBeI16(bytes, 0x00);
  // Recode the voice code by range (see loader 0x004bd3c0): 0..7 keep mode -1;
  // 1000..1007 and 2000..2007 subtract the offset and set mode 1/0; any other
  // value marks both fields -1.
  const std::int16_t vtc = g.voice_type_code;
  if (vtc >= 0 && vtc <= 7) {
    g.voice_type_mode = -1;
  } else if (vtc >= 1000 && vtc <= 1007) {
    g.voice_type_code = vtc - 1000;
    g.voice_type_mode = 1;
  } else if (vtc >= 2000 && vtc <= 2007) {
    g.voice_type_code = vtc - 2000;
    g.voice_type_mode = 0;
  } else {
    g.voice_type_code = -1;
    g.voice_type_mode = -1;
  }

  g.flags_primary = ReadBe16(bytes, 0x02);
  g.scan_mask_short = ReadBe16(bytes, 0x04);
  g.inherent_jam[0] = ReadBeI16(bytes, 0x06);
  g.flee_shield_threshold = ReadBeI16(bytes, 0x08);
  g.disable_penalty = ReadBeI16(bytes, 0x0a);
  g.board_penalty = ReadBeI16(bytes, 0x0c);
  g.kill_penalty = ReadBeI16(bytes, 0x0e);
  g.shoot_penalty = ReadBeI16(bytes, 0x10);
  g.max_odds = ReadBeI16(bytes, 0x12);
  g.bribe_cost_percent = ReadBeI16(bytes, 0x14);

  // combat_rating_scale = int16(payload +0x16) * 0.01; degenerate -> 0.01.
  g.combat_rating_scale = static_cast<float>(ReadBeI16(bytes, 0x16)) * 0.01F;
  if (g.combat_rating_scale < 0.01F) {
    g.combat_rating_scale = 0.01F;
  }

  for (std::size_t i = 0; i < 4; ++i) {
    g.classes[i] = ReadBeI16(bytes, 0x18 + i * 2);
    g.ally_classes[i] = ReadBeI16(bytes, 0x20 + i * 2);
    g.enemy_classes[i] = ReadBeI16(bytes, 0x28 + i * 2);
  }

  // pilot_skill_scale = int16(payload +0x30) * 0.01; source < 1 -> 1.0.
  const std::int16_t pilot_src = ReadBeI16(bytes, 0x30);
  g.pilot_skill_scale = static_cast<float>(pilot_src) * 0.01F;
  if (pilot_src < 1) {
    g.pilot_skill_scale = 1.0F;
  }
  g.ai_skill_percent = ReadBeI16(bytes, 0x32);

  g.comm_name = ReadCString(bytes, 0x34);
  // Payload +0x44 is the TargetCode short string (g_government_name_table in
  // the loader); the display name comes from the resource record name instead.
  g.target_code = ReadCString(bytes, 0x44);
  g.medium_name = ReadCString(bytes, 0x64);

  g.scan_mask_lo = ReadBe32(bytes, 0x54);
  g.scan_mask_hi = ReadBe32(bytes, 0x58);
  // jam[1..3] from +0x5c, each clamped to [0,100].
  for (std::size_t i = 1; i < 4; ++i) {
    g.inherent_jam[i] = ReadBeI16(bytes, 0x5c + (i - 1) * 2);
    if (g.inherent_jam[i] > 100) {
      g.inherent_jam[i] = 100;
    } else if (g.inherent_jam[i] < 0) {
      g.inherent_jam[i] = 0;
    }
  }

  // Theme/ship colors are packed RGB24 words at +0xa4/+0xa8. The loader
  // spreads each byte into a 16-bit (<<8) field and mirrors the >>8 8-bit
  // values; we carry the final 8-bit colors directly.
  const std::uint32_t theme = ReadBe32(bytes, 0xa4);
  g.theme_red = static_cast<std::uint8_t>((theme >> 16U) & 0xffU);
  g.theme_green = static_cast<std::uint8_t>((theme >> 8U) & 0xffU);
  g.theme_blue = static_cast<std::uint8_t>(theme & 0xffU);
  const std::uint32_t ship = ReadBe32(bytes, 0xa8);
  g.ship_red = static_cast<std::uint8_t>((ship >> 16U) & 0xffU);
  g.ship_green = static_cast<std::uint8_t>((ship >> 8U) & 0xffU);
  g.ship_blue = static_cast<std::uint8_t>(ship & 0xffU);

  g.interface_id = ReadBeI16(bytes, 0xac);
  if (g.interface_id < 0x80) {
    g.interface_id = -1;
  }
  g.news_pic_id = ReadBeI16(bytes, 0xae);
  if (g.news_pic_id < 0x80) {
    g.news_pic_id = -1;
  }

  g.present = true;
  return g;
}

// ---------------------------------------------------------------------------
// s\xd8st (System / s\xffst) decode
// ---------------------------------------------------------------------------
// Header verified: xPos+0, yPos+2, Con1-16+0x04, NavDef1-16+0x24,
// Dude1-8+0x44, DudeProb1-8+0x54, AvgShips+0x64, govt+0x66, Message+0x68,
// Asteroids+0x6a, Interference+0x6c, DudeTypes+0x6e, % Prob+0x7e,
// BkgndColor+0x8e (24-bit RRGGBB, Ghidra
// NovaData_LoadScenario- ResourceTables reads a 32-bit at payload +0x8e and
// splits the three bytes into SystemDef.field_0x1ee/.f0/.f2), Murk+0x92 (feeds
// SystemDef.murk at +0xbc; the payload name "alert_level" is a misnomer - the
// EV Nova Bible documents Murk as the starfield/ambience
// opacity, negative hides the starfield). The loader also reads DudeTypes/Prob
// (8 shorts each, with id rebasing/prob clamping), ReinfFleet/Time/Intrval near
// the end of the record, plus the Visibility string.
[[nodiscard]] System DecodeSystem(std::span<const std::byte> bytes) {
  System s;
  s.pos_x = ReadBeI16(bytes, 0x00);
  s.pos_y = ReadBeI16(bytes, 0x02);
  // Ghidra NovaData_LoadScenarioResourceTables 0x004bd3c0 sets is_visible and
  // has_explored_flag to 1 for every decoded syst: they are LOADED/availability
  // flags, not per-visit fog (the fog record is discovery_state below).
  // NovaResources_EvaluateAvailability 0x00448090 later re-filters is_visible
  // through the system's Visibility NCB when mission lists are evaluated.
  s.is_visible = true;
  s.has_explored_flag = true;
  for (std::size_t i = 0; i < s.links.size(); ++i) {
    s.links[i] = ReadBeI16(bytes, 0x04 + i * 2);
    s.nav_defs[i] = ReadBeI16(bytes, 0x24 + i * 2);
  }
  // The loader splits the Dude1-8 table into ordinary dude-class bindings and
  // random-encounter fleet bindings. Positive resource ids 0x80..0x27f become
  // zero-based dude-class ids. Negative references -0x80..-0x17f encode fleet
  // ids as abs(raw)-0x80; their weights also sum to encounter_chance_percent.
  std::uint16_t dude_weight_total = 0;
  for (std::size_t i = 0; i < s.dude_class_ids.size(); ++i) {
    const std::int16_t raw_id = ReadBeI16(bytes, 0x44 + i * 2);
    const std::uint16_t weight = ReadBe16(bytes, 0x54 + i * 2);
    if (raw_id >= 0x80 && raw_id <= 0x27f) {
      s.dude_class_ids[i] = static_cast<std::int16_t>(raw_id - 0x80);
      s.dude_class_weights[i] = weight;
      dude_weight_total =
          static_cast<std::uint16_t>(dude_weight_total + weight);
    } else if (raw_id <= -0x80 && raw_id > -0x180 &&
               s.encounter_fleet_count <
                   static_cast<std::int16_t>(s.encounter_fleet_ids.size())) {
      const std::size_t fleet_slot =
          static_cast<std::size_t>(s.encounter_fleet_count);
      s.encounter_fleet_ids[fleet_slot] =
          static_cast<std::int16_t>(-raw_id - 0x80);
      s.encounter_fleet_weights[fleet_slot] = weight;
      ++s.encounter_fleet_count;
      s.encounter_chance_percent = static_cast<std::int16_t>(
          s.encounter_chance_percent + static_cast<std::int16_t>(weight));
    }
  }
  // Ordinary dude weights are normalized to a 100-point distribution when
  // the payload does not already sum to 100. Fleet weights are excluded.
  if (dude_weight_total != 0 && dude_weight_total != 100) {
    const float scale = 100.0F / static_cast<float>(dude_weight_total);
    for (std::uint16_t &weight : s.dude_class_weights) {
      weight = static_cast<std::uint16_t>(
          std::lround(static_cast<float>(weight) * scale));
    }
  }
  // AvgShips/Govt/Message/Asteroids/Interference block (payload +0x64..+0x6c),
  // verified against the loader (0x004bd3c0): avg_ships +0x64, government
  // +0x66, Message +0x68, Asteroids +0x6a, Interference +0x6c.
  s.avg_ships = ReadBeI16(bytes, 0x64);
  s.government_id = ReadBeI16(bytes, 0x66);
  s.message_id = ReadBeI16(bytes, 0x68);
  // Payload +0x6a -> SystemDef.asteroid_count (+0x94). The Asteroid field
  // (number of asteroid/drift records, 0-16); read by Asteroid_InitSystem
  // (0x004216B0) / Asteroid_Spawn (0x00421830).
  s.asteroid_count = ReadBeI16(bytes, 0x6a);
  s.interference = ReadBeI16(bytes, 0x6c);
  // Government rebase mirrors the loader: < 0x80 or > 0x17f -> -1 else -0x80.
  if (s.government_id < 0x80 || s.government_id > 0x17f) {
    s.government_id = -1;
  } else {
    s.government_id = static_cast<std::int16_t>(s.government_id - 0x80);
  }
  for (std::size_t i = 0; i < s.dude_types.size(); ++i) {
    s.dude_types[i] = ReadBeI16(bytes, 0x6e + i * 2);
    s.dude_prob[i] = ReadBeI16(bytes, 0x7e + i * 2);
    // Dude type id rebase/clamp (loader): < 0x80 or > 0x47e -> -1 else -0x80.
    if (s.dude_types[i] < 0x80 || s.dude_types[i] > 0x47e) {
      s.dude_types[i] = -1;
    } else {
      s.dude_types[i] = static_cast<std::int16_t>(s.dude_types[i] - 0x80);
    }
    // % Prob clamp to [0,100] (loader).
    if (s.dude_prob[i] < 0) {
      s.dude_prob[i] = 0;
    } else if (s.dude_prob[i] > 100) {
      s.dude_prob[i] = 100;
    }
  }
  // BkgndColor (s\xd8st +0x8e): three colour bytes (pure black when unset) that
  // the original reads as a 32-bit little-endian value and splits into the
  // SystemDef.field_0x1ee/.f0/.f2 shorts, mapping R = resource byte +0x90,
  // G = +0x8f, B = +0x8e (a byte-order quirk of the 32-bit read that visibly
  // affects the rendered colour -- preserved for fidelity). This is the
  // per-system space background tint used by
  // NovaRender_SetSystemSpaceBackgroundColor (Ghidra 0x0046bbf0).
  const std::uint32_t bkgnd = ReadBe32(bytes, 0x8e); // mem[0x8e..0x91]
  s.bkgnd_color = ((bkgnd >> 8) & 0xff) << 16 |      // 0x90 -> R
                  ((bkgnd >> 16) & 0xff) << 8 |      // 0x8f -> G
                  ((bkgnd >> 24) & 0xff);            // 0x8e -> B
  // Murk (s\xd8st +0x92): murkiness 0-100 (SystemDef.murk at +0xbc; the
  // payload name "alert_level" is a misnomer); a negative value
  // equivalently hides the starfield (NovaEffects_QueuedAmbientStarParticles
  // clears ambient stars when SystemDef.murk < 0).
  s.murk = ReadBeI16(bytes, 0x92);
  // AstTypes (s\xd8st +0x94): a 16-bit mask of allowed asteroid types. The
  // loader copies it verbatim into SystemDef.ast_types (+0x1f4);
  // Asteroid_Spawn (0x00421830) tests it via
  // (1 << (wander_type & 0x1f)) & ast_types.
  s.ast_types = static_cast<std::uint16_t>(ReadBeI16(bytes, 0x94));
  const std::int16_t reinf_fleet = ReadBeI16(bytes, 0x196);
  s.reinf_fleet =
      reinf_fleet < 0x80 ? -1 : static_cast<std::int16_t>(reinf_fleet - 0x80);
  s.reinf_time = ReadBeI16(bytes, 0x198);
  s.reinf_interval = ReadBeI16(bytes, 0x19a);
  if (s.reinf_fleet != -1 && s.reinf_interval < 1) {
    s.reinf_interval = 1;
  }
  s.visibility_expr = ReadCString(bytes, 0x96);
  return s;
}

// ---------------------------------------------------------------------------
// fl\x91t (RandomEncounterFleetDef) decode
// ---------------------------------------------------------------------------
// Offsets verified against the loader's fleet section (0x004bd3c0):
// lead_ship_class payload +0x00, escort_ship_class_ids[4] +0x02,
// escort_min_count[4] +0x0a, escort_max_count[4] +0x12, government_id +0x1a,
// spawn_system_filter +0x1c, availability_expression +0x1e, arrival_message_id
// +0x11e, carry_cargo_flags +0x120. The loader rebases every id that is >= 0x80
// down by 0x80 and nulls the fields whose id reads < 0x80: lead id < 0x80
// becomes 0 (and the def never spawns), government id < 0x80 becomes -1, and a
// < 0x80 escort class id invalidates that escort slot (-1 with min/max zeroed).
// is_available_runtime is runtime-only (starts false); the availability string
// is left as read.
[[nodiscard]] FleetDef DecodeFleet(std::span<const std::byte> bytes) {
  FleetDef f;

  f.lead_ship_class_id = ReadBeI16(bytes, 0x00);
  f.government_id = ReadBeI16(bytes, 0x1a);
  f.spawn_system_filter = ReadBeI16(bytes, 0x1c);
  f.arrival_message_id = ReadBeI16(bytes, 0x11e);
  f.flags = ReadBe16(bytes, 0x120);

  for (std::size_t i = 0; i < 4; ++i) {
    f.escort_ship_class_ids[i] = ReadBeI16(bytes, 0x02 + i * 2);
    f.escort_min_count[i] = ReadBeI16(bytes, 0x0a + i * 2);
    f.escort_max_count[i] = ReadBeI16(bytes, 0x12 + i * 2);
  }
  f.availability_expr = ReadCString(bytes, 0x1e);

  // Rebasing mirrors the loader's per-id normalization. A < 0x80 lead
  // means "no fleet" and is modelled as -1 (matching the original's
  // `lead_ship_class_id == -1` spawn gate in EncounterFleet_SpawnRandom-
  // EncounterFleet 0x004259b0; the loader's transient store of 0 for that
  // case is unused at spawn time).
  if (f.lead_ship_class_id < 0x80) {
    f.lead_ship_class_id = -1;
  } else {
    f.lead_ship_class_id =
        static_cast<std::int16_t>(f.lead_ship_class_id - 0x80);
  }
  if (f.government_id < 0x80) {
    f.government_id = -1;
  } else {
    f.government_id = static_cast<std::int16_t>(f.government_id - 0x80);
  }
  for (std::size_t i = 0; i < 4; ++i) {
    if (f.escort_ship_class_ids[i] < 0x80) {
      f.escort_ship_class_ids[i] = -1;
      f.escort_min_count[i] = 0;
      f.escort_max_count[i] = 0;
    } else {
      f.escort_ship_class_ids[i] =
          static_cast<std::int16_t>(f.escort_ship_class_ids[i] - 0x80);
    }
  }

  return f;
}

// ---------------------------------------------------------------------------
// d\x9fde (DudeDef) decode
// ---------------------------------------------------------------------------
// See the DudeDef comment in scenario_data.hpp for the payload layout.
// This mirrors the original loader's dude section (NovaData_LoadScenario-
// ResourceTables 0x004bd3c0, at 0x004c2a2d..), which pulls the payload's
// scattered fields into the 74-byte in-memory DudeDef, big-endian 16-bit
// reads, and re-bases / validates the id references. `ship_table` is the
// already-decoded ship-class table (g_ship_class_defs) used to null out a
// ship type whose target class carries the "nonexistent" marker (a tech_level
// of (short)0xd8f1 == -9999). The payload d\x9fde records observed in the
// shipped archives are 88 bytes, matching the loader's EnsureBlockSize(payload,
// 0x58); every shipped record references only defined ship classes, so the
// marker branch never trips on real data.
[[nodiscard]] DudeDef DecodeDude(std::span<const std::byte> bytes,
                                 const std::vector<ShipClass> &ship_table) {
  DudeDef d;
  // The loader only accepts an 88-byte (0x58) minimum payload; bail to an
  // empty def when the record is truncated.
  if (bytes.size() < 0x58) {
    return d;
  }

  d.ai_type = ReadBeI16(bytes, 0x00);
  d.government_id = ReadBeI16(bytes, 0x02);
  if (d.government_id >= 0x80 && d.government_id < 0x180) {
    d.government_id = static_cast<std::int16_t>(d.government_id - 0x80);
  }
  d.booty_flags = ReadBe16(bytes, 0x04);
  d.hail_info_types = ReadBe16(bytes, 0x06);

  for (std::size_t i = 0; i < d.ship_types.size(); ++i) {
    std::int16_t ship = ReadBeI16(bytes, 0x08 + i * 2);
    if (ship >= 0x80 && ship < 0x380) {
      ship = static_cast<std::int16_t>(ship - 0x80);
      // Original: CMP g_ship_class_defs[ship].tech_level (ShipClassDef +0xa),
      // 0xd8f1 -> set the slot to -1. tech_level is 16-bit, so (short)0xd8f1
      // is the -9999 sentinel; real data never carries it (verified against
      // the shipped archives), but replicate the gate for fidelity. An
      // out-of-range class index (should not occur after the -0x80 rebase) is
      // also nulled.
      if (ship >= 0 && static_cast<std::size_t>(ship) < ship_table.size() &&
          ship_table[static_cast<std::size_t>(ship)].tech_level ==
              kShipClassNonexistentTechLevel) {
        ship = -1;
      }
    }
    d.ship_types[i] = ship;
  }
  for (std::size_t i = 0; i < d.ship_probabilities.size(); ++i) {
    d.ship_probabilities[i] = ReadBeI16(bytes, 0x28 + i * 2);
  }
  d.present = true;
  return d;
}

// ---------------------------------------------------------------------------
// r\x9aid (AsteroidDef) decode
// ---------------------------------------------------------------------------
// See AsteroidDef in scenario_data.hpp. Mirrors the original loader's
// asteroid-type section (NovaData_LoadScenarioResourceTables 0x004bd3c0 at
// 0x004c6207..), which reads the record's fields into the shared 0x1c-row
// table exposed as DAT_005912dc / DAT_005912f0. Fields are big-endian 16-bit
// except the 4-byte colour word at +0x0a (byte-swapped to a packed 15-bit RGB
// at +0x18). Validation branches match the loader (skip row on a bad id
// window); harmless for the shipped records.
[[nodiscard]] AsteroidDef DecodeAsteroidType(std::span<const std::byte> bytes) {
  AsteroidDef t;
  if (bytes.size() < 0x18) {
    return t;
  }
  t.wander_table_value = ReadBeI16(bytes, 0x00);
  // word[0x2] * 0.01 -> float multiplier (the loader FMULs by the 0.01 double
  // DAT_00575e60).
  t.wander_speed_multiplier =
      static_cast<float>(ReadBeI16(bytes, 0x02)) * 0.01F;
  t.field_0x04 = ReadBeI16(bytes, 0x04);
  t.field_0x02 = ReadBeI16(bytes, 0x06);
  t.field_0x0c = ReadBeI16(bytes, 0x08);
  // Colour: the loader squashes the three RGB565-ish bytes at +0x0a to a
  // 15-bit tint: red=byte[0xc]>>3, green=byte[0xb]>>3, blue=byte[0xa]>>3
  // (see 0x004c62ed..0x004c6380).
  {
    const auto b0 = std::to_integer<std::uint8_t>(bytes[0x0a]); // blue
    const auto b1 = std::to_integer<std::uint8_t>(bytes[0x0b]); // green
    const auto b2 = std::to_integer<std::uint8_t>(bytes[0x0c]); // red
    t.color = (static_cast<std::uint32_t>(b2) >> 3U) << 10U |
              (static_cast<std::uint32_t>(b1) >> 3U) << 5U |
              (static_cast<std::uint32_t>(b0) >> 3U);
  }
  // 3-element direction sub-array at payload +0x0e; the loader rebases
  // 0x80..0x90 by -0x80, else requires the value < 0x10.
  for (std::size_t i = 0; i < t.directions.size(); ++i) {
    std::int16_t v = ReadBeI16(bytes, 0x0e + i * 2);
    if (v >= 0x80 && v < 0x90) {
      v = static_cast<std::int16_t>(v - 0x80);
    }
    // Out-of-range (non <0x10, non 0x80..0x90) would abort the row in the
    // original; shipped data stays within the accepted windows.
    t.directions[i] = v;
  }
  t.field_0x10 = ReadBeI16(bytes, 0x14);
  t.lifetime = ReadBeI16(bytes, 0x16);
  t.present = true;
  return t;
}

} // namespace

const ShipClass *ScenarioData::Ship(std::int16_t resource_id) const {
  const auto index = static_cast<std::size_t>(resource_id) - 0x80;
  return index < ships.size() ? &ships[index] : nullptr;
}

const Outfit *ScenarioData::Outfit(std::int16_t resource_id) const {
  const auto index = static_cast<std::size_t>(resource_id) - 0x80;
  return index < outfits.size() ? &outfits[index] : nullptr;
}

const Weapon *ScenarioData::Weapon(std::int16_t resource_id) const {
  const auto index = static_cast<std::size_t>(resource_id) - 0x80;
  return index < weapons.size() ? &weapons[index] : nullptr;
}

const Stellar *ScenarioData::Stellar(std::int16_t resource_id) const {
  const auto index = static_cast<std::size_t>(resource_id) - 0x80;
  return index < stellars.size() ? &stellars[index] : nullptr;
}

const System *ScenarioData::System(std::int16_t resource_id) const {
  const auto index = static_cast<std::size_t>(resource_id) - 0x80;
  return index < systems.size() ? &systems[index] : nullptr;
}

const Government *ScenarioData::Government(std::int16_t resource_id) const {
  const auto index = static_cast<std::size_t>(resource_id) - 0x80;
  return index < governments.size() ? &governments[index] : nullptr;
}

const Government *ScenarioData::GovernmentByIndex(std::int16_t index) const {
  return index >= 0 && static_cast<std::size_t>(index) < governments.size()
             ? &governments[static_cast<std::size_t>(index)]
             : nullptr;
}

const FleetDef *ScenarioData::Fleet(std::int16_t resource_id) const {
  const auto index = static_cast<std::size_t>(resource_id) - 0x80;
  return index < fleets.size() ? &fleets[index] : nullptr;
}

const DudeDef *ScenarioData::Dude(std::int16_t resource_id) const {
  const auto index = static_cast<std::size_t>(resource_id) - 0x80;
  return index < dudes.size() ? &dudes[index] : nullptr;
}

const MissionDef *ScenarioData::Mission(std::int16_t resource_id) const {
  const auto index = static_cast<std::int32_t>(resource_id) - 0x80;
  if (index < 0 || index >= static_cast<std::int32_t>(missions.size())) {
    return nullptr;
  }
  return &missions[static_cast<std::size_t>(index)];
}

const PersDef *ScenarioData::Pers(std::int16_t resource_id) const {
  const auto index = static_cast<std::int32_t>(resource_id) - 0x80;
  if (index < 0 || index >= static_cast<std::int32_t>(pers_defs.size())) {
    return nullptr;
  }
  return &pers_defs[static_cast<std::size_t>(index)];
}

const AsteroidDef *ScenarioData::AsteroidType(std::int16_t resource_id) const {
  const auto index = static_cast<std::size_t>(resource_id) - 0x80;
  return index < asteroid_defs.size() ? &asteroid_defs[index] : nullptr;
}

const AsteroidDef *
ScenarioData::ImpactPackageAt(std::int16_t package_id) const {
  if (package_id < 0 || package_id >= static_cast<std::int16_t>(0x10)) {
    return nullptr;
  }
  return &asteroid_defs[static_cast<std::size_t>(package_id)];
}

const ImpactEffect *ScenarioData::ImpactEffectAt(std::int16_t effect_id) const {
  if (effect_id < 0 ||
      effect_id >= static_cast<std::int16_t>(impact_effects.size())) {
    return nullptr;
  }
  return &impact_effects[static_cast<std::size_t>(effect_id)];
}

bool ScenarioData::LoadFromArchives() {
  // The original sizes these tables to the family maximum and zero-fills
  // missing slots (0x200 ships/outfits, 0x100 weapons). We mirror that so
  // callers can index directly by id - 0x80.
  ships.assign(0x200, {});
  outfits.assign(0x200, {});
  weapons.assign(0x100, {});
  stellars.assign(0x600, {});
  systems.assign(0x800, {});
  // GovernmentDef table is capped at 0x100 entries by the original (loop bound
  // sVar21 < 0x100); federal classes are indexed by government id minus 0x80.
  governments.assign(0x100, {});
  // Random-encounter fleet tables: the original loops < 0x100 entries indexed
  // by fleet id minus 0x80.
  fleets.assign(0x100, {});
  // Dude table (g_dude_defs): the original runs up to < 0x200 resource slots
  // (the loader's loop bound at 0x004c2c81) at a 0x4a-byte DudeDef stride,
  // indexed by dude id minus 0x80.
  dudes.assign(0x200, {});
  // The original mission definition table has 1000 entries, indexed by
  // resource id minus 0x80 (NovaResources_LoadMisnResourceDefs 0x0043bbb0).
  missions.assign(1000, {});
  // p\x91rs personality table (g_pers_defs): 0x400 slots, slot i =
  // resource id 0x80 + i. Absent ids keep inactive rows, matching the
  // original's zero-filled table.
  pers_defs.assign(0x400, {});
  // crön events: 0x200 slots, resource ids 0x80..0x27f. Absent ids keep
  // !present rows, matching the loader's 0xffff duration sentinel.
  cron_events.assign(0x200, {});
  // Asteroid-type (asteroid-drift) table: 16 rows, resource ids 0x80..0x8f.
  asteroid_defs.assign(0x80, {});

  // Impact definitions are a fixed 64-entry runtime table, zero-filled by
  // the original before it probes source ids 0x80..0xbf.
  impact_effects.fill({});

  std::size_t loaded_ships = 0;
  std::size_t loaded_weapons = 0;
  std::size_t loaded_outfits = 0;
  std::size_t loaded_stellars = 0;
  std::size_t loaded_systems = 0;
  std::size_t loaded_governments = 0;
  std::size_t loaded_fleets = 0;
  std::size_t loaded_dudes = 0;
  std::size_t loaded_missions = 0;
  std::size_t loaded_asteroid_types = 0;
  std::size_t loaded_impact_effects = 0;

  // BaseImageID -> first zero-based ship class using it, for clone-source
  // derivation (ShipClass_LoadShipClassVisualAndLaunchData 0x004b4ee0's clone
  // branch: a class whose sh\x8an BaseImageID matches an earlier class's
  // reuses that class's sprites and target portrait).
  std::map<std::uint16_t, std::int16_t> first_class_by_base_image;
  for (std::int32_t id = 0x80; id <= 0x27f; ++id) {
    if (const auto res = NovaResource_LoadNamed(
            scenario::kShipResourceType, static_cast<std::uint16_t>(id))) {
      ShipClass cls = DecodeShip(res->bytes);
      // The ship class display name is the resource record name with the
      // ';'-subtitle suffix stripped (the loader fills ShipClassDef+0x6c via
      // ResourceData_ReadEntryMetadata + NameString_StripSubtitleSuffix
      // 0x004cd230, bounded to 0x3f chars), not a numeric header field.
      cls.display_name = StripSubtitleSuffix(res->name);
      const std::size_t index = static_cast<std::size_t>(id) - 0x80;
      // Clone-source derivation: the sh\x8an descriptor shares the class id,
      // and its BaseImageID (+0x00) is the sheet the class's sprites are cut
      // from. The first class (lowest id) seen with a given BaseImageID is the
      // clone source; identical-looking classes get its portrait (Bible:
      // "PICT resource ID 3000 + shipID - 128 ... for all higher-numbered
      // ship types with the same base sprites").
      const auto shan = NovaResource_Load(kShipVisualResourceType,
                                          static_cast<std::uint16_t>(id));
      if (shan && shan->size() >= 2) {
        const std::uint16_t base_image = ReadBe16(*shan, 0x00);
        // Ghidra ShipClass_LoadShipClassVisualAndLaunchData (0x004b4ee0)
        // copies sh\x8an +0x2e into ShipClassDef +0xa24. Bible Flags 0x0001:
        // "extra frames in base image are used to display banking. The first
        // set of sprites is used for level flight, the second for banking
        // left, and the third for banking right." Consumers: the turn-bank
        // bias blocks, the waypoint-marker arm (weapon.cpp), and the combat-
        // animation cycle.
        cls.sprite_behavior_flags = ReadBe16(*shan, 0x2e);
        // Rotation frame count (ShipClassDef +0xa06): sh\x8an +0x34 FramesPer,
        // 36 when 0 (ShipClass_LoadShipClassVisualAndLaunchData default).
        {
          const std::int16_t frames = ReadBeI16(*shan, 0x34);
          cls.frames_per_rotation = frames != 0 ? frames : 36;
        }
        // Weapon-exit (muzzle) geometry. Loader copy map (0x004b4ee0,
        // sh\x8an -> ShipClassDef +0xa42..+0xab0) combined with the barrel
        // indexing of Weapon_ApplyTurretSpreadVelocity (0x0046c5c0),
        // i = turret_group*4 + quadrant:
        //   lateral[g][q] = sh\x8an 0x48 + 8g + 2q  (class +0xa42 + 4i)
        //   forward[g][q] = sh\x8an 0x50 + 8g + 2q  (class +0xa44 + 4i)
        //   drop[g][q]    = sh\x8an 0x90 + 8g + 2q  (class +0xa82 + 2i)
        // Reads past the payload are zero (the loader pads the descriptor
        // block to 0xc0 before copying).
        auto muzzle_read = [&shan](int offset) -> std::int16_t {
          return offset + 2 <= static_cast<int>(shan->size())
                     ? ReadBeI16(*shan, static_cast<std::size_t>(offset))
                     : static_cast<std::int16_t>(0);
        };
        for (int g = 0; g < 4; ++g) {
          for (int q = 0; q < 4; ++q) {
            cls.muzzle_lateral[g][q] = muzzle_read(0x48 + 8 * g + 2 * q);
            cls.muzzle_forward[g][q] = muzzle_read(0x50 + 8 * g + 2 * q);
            cls.muzzle_drop[g][q] = muzzle_read(0x90 + 8 * g + 2 * q);
          }
        }
        // Near/far compress scales (class +0xaa4/+0xaa8 and +0xaac/+0xab0):
        // raw sh\x8an short * g_ship_weapon_exit_compress_scale (0x00575ac0 =
        // 0.01f), each clamped to 1.0 when <= 0 (loader tail).
        auto muzzle_scale = [&muzzle_read](int offset) -> float {
          const float value = static_cast<float>(muzzle_read(offset)) * 0.01F;
          return value <= 0.0F ? 1.0F : value;
        };
        cls.muzzle_scale_near_x = muzzle_scale(0x88);
        cls.muzzle_scale_near_y = muzzle_scale(0x8a);
        cls.muzzle_scale_far_x = muzzle_scale(0x8c);
        cls.muzzle_scale_far_y = muzzle_scale(0x8e);
        cls.muzzle_ready = true;
        // Combat/sprite animation cadence seed (ShipClassDef +0x9fe <- sh\x8an
        // +0x30, loader 0x004b4ee0); drawn by the spawn paths.
        cls.combat_state_init_range = ReadBeI16(*shan, 0x30);
        if (const auto found = first_class_by_base_image.find(base_image);
            found != first_class_by_base_image.end()) {
          cls.clone_source_ship_class = found->second;
          // Escort type is the clone source class (ShipClassDef +0xa08); the
          // original leaves it at -1 when the sprite is built fresh.
          cls.escort_type = found->second;
        } else {
          cls.clone_source_ship_class = static_cast<std::int16_t>(index);
          first_class_by_base_image.emplace(base_image,
                                            static_cast<std::int16_t>(index));
        }
      } else {
        // No sh\x8an descriptor: the class owns its (missing) sprites.
        cls.clone_source_ship_class = static_cast<std::int16_t>(index);
      }
      // Large 200x200 portrait PICT for the ship-comm / shipyard panels
      // (ShipClassDef +0xA0A). Ghidra NovaData_LoadAllShipClassVisualAndLaunch
      // Data (0x004aeda0): use PICT `index + 5000` when that resource exists
      // (FUN_004ce640(CICN 'PICT', index + 5000)), else fall back to the
      // clone-source class's portrait `clone_source_ship_class + 5000` (the
      // portrait resource lives at 5000 + a zero-based class id).
      const std::uint16_t own_portrait =
          static_cast<std::uint16_t>(index + 5000);
      cls.pict_fallback_sprite_resource_id =
          NovaResource_Load(kResourceTypePict, own_portrait)
              ? own_portrait
              : static_cast<std::uint16_t>(cls.clone_source_ship_class + 5000);
      ships[index] = std::move(cls);
      ++loaded_ships;
    }
  }
  for (std::int32_t id = 0x80; id <= 0x27f; ++id) {
    if (const auto res = NovaResource_LoadNamed(
            scenario::kOutfitResourceType, static_cast<std::uint16_t>(id))) {
      game::Outfit outfit = DecodeOutfit(res->bytes);
      outfit.name = res->name; // record name (HUD/UI display)
      outfits[static_cast<std::size_t>(id) - 0x80] = std::move(outfit);
      ++loaded_outfits;
    }
  }
  for (std::int32_t id = 0x80; id <= 0x17f; ++id) {
    if (const auto res = NovaResource_LoadNamed(
            scenario::kWeaponResourceType, static_cast<std::uint16_t>(id))) {
      game::Weapon weapon = DecodeWeapon(res->bytes);
      weapon.name = res->name;
      weapons[static_cast<std::size_t>(id) - 0x80] = std::move(weapon);
      ++loaded_weapons;
    }
  }
  ComputeWeaponEffectiveRanges(weapons);
  for (std::int32_t id = 0x80; id <= 0x57f; ++id) {
    if (const auto res = NovaResource_LoadNamed(
            scenario::kStellarResourceType, static_cast<std::uint16_t>(id))) {
      game::Stellar st = DecodeStellar(res->bytes);
      st.name = res->name;
      stellars[static_cast<std::size_t>(id) - 0x80] = std::move(st);
      ++loaded_stellars;
    }
  }
  for (std::int32_t id = 0x80; id <= 0x47f; ++id) {
    if (const auto res = NovaResource_LoadNamed(
            scenario::kSystemResourceType, static_cast<std::uint16_t>(id))) {
      game::System sys = DecodeSystem(res->bytes);
      sys.name = res->name;
      systems[static_cast<std::size_t>(id) - 0x80] = std::move(sys);
      ++loaded_systems;
    }
  }

  // Ghidra 0x004beb4f (inside NovaData_LoadScenarioResourceTables
  // 0x004bd3c0): visibility-twin grouping. The scenario repeats syst
  // resources at identical map positions for story-driven government swaps
  // (e.g. Koria Federation/rebel-assimilated, the whole Vell-os b330 region);
  // the first decoded id at each position becomes the group ROOT and the
  // rest chain off it via visible_parent_system_id, so
  // System_ResolveSystemDiscoverySlot / System_ResolveVisibleSystemFor-
  // Travel (0x0046b9b0 / 0x0046b920) can pick whichever twin's Visibility
  // NCB currently holds. Runs while is_visible is still the loader-set
  // "syst decoded" flag for every entry, exactly like the original.
  auto &system_table = systems;
  for (std::size_t i = 0; i < system_table.size(); ++i) {
    system_table[i].visible_parent_system_id = -1;
    system_table[i].visibility_root_system_id = -1;
  }
  for (std::size_t i = 0; i < system_table.size(); ++i) {
    if (!system_table[i].has_explored_flag ||
        system_table[i].visibility_root_system_id != -1) {
      continue;
    }
    system_table[i].visibility_root_system_id = static_cast<std::int16_t>(i);
    std::int16_t chain = static_cast<std::int16_t>(i);
    for (std::size_t j = 0; j < system_table.size(); ++j) {
      if (!system_table[j].has_explored_flag ||
          system_table[j].pos_x != system_table[i].pos_x ||
          system_table[j].pos_y != system_table[i].pos_y) {
        continue;
      }
      // Already grouped under an earlier root (only possible for j < i,
      // whose group claimed this position first).
      if (system_table[j].visibility_root_system_id != -1 && !(i < j)) {
        continue;
      }
      system_table[static_cast<std::size_t>(chain)].visible_parent_system_id =
          static_cast<std::int16_t>(j);
      system_table[j].visibility_root_system_id = static_cast<std::int16_t>(i);
      chain = static_cast<std::int16_t>(j);
    }
  }

  // Link normalization (0x004bd3c0, after the grouping pass). Pass 1: prune
  // Con slots pointing at ids with no decoded syst (the loader logs these;
  // the clean-room logs a terse note). Out-of-range raw Con values (the
  // loader rebases only 0x80..0x87f, else -1) become -1 here too.
  for (std::size_t i = 0; i < system_table.size(); ++i) {
    if (!system_table[i].has_explored_flag) {
      continue;
    }
    for (std::int16_t &link : system_table[i].links) {
      const std::int16_t target = static_cast<std::int16_t>(link - 0x80);
      if (link < 0x80 || target < 0 ||
          static_cast<std::size_t>(target) >= system_table.size() ||
          !system_table[static_cast<std::size_t>(target)].has_explored_flag) {
        if (link >= 0x80) {
          NovaLog::Info(
              "system 0x{:x}: pruning link 0x{:x} (no syst)", i + 0x80, link);
        }
        link = -1;
      }
    }
  }
  // Pass 2: remap every link to its target's visibility ROOT
  // (System_ResolveSystemDiscoverySlot 0x0046b9b0), drop links resolving to
  // the source's own discovery slot, and deduplicate repeated targets.
  const auto discovery_slot = [&system_table](std::int16_t id) {
    if (id < 0 || static_cast<std::size_t>(id) >= system_table.size()) {
      return static_cast<std::int16_t>(-1);
    }
    const std::int16_t root =
        system_table[static_cast<std::size_t>(id)].visibility_root_system_id;
    return root != -1 ? root : id;
  };
  for (std::size_t i = 0; i < system_table.size(); ++i) {
    if (!system_table[i].has_explored_flag) {
      continue;
    }
    const std::int16_t self_slot = discovery_slot(static_cast<std::int16_t>(i));
    for (std::size_t k = 0; k < system_table[i].links.size(); ++k) {
      std::int16_t &link = system_table[i].links[k];
      if (link < 0x80) {
        continue;
      }
      const std::int16_t resolved =
          discovery_slot(static_cast<std::int16_t>(link - 0x80));
      bool duplicate = resolved == self_slot;
      for (std::size_t prev = 0; prev < k && !duplicate; ++prev) {
        const std::int16_t prev_target =
            static_cast<std::int16_t>(system_table[i].links[prev] - 0x80);
        if (system_table[i].links[prev] >= 0x80 &&
            discovery_slot(prev_target) == resolved) {
          duplicate = true;
        }
      }
      if (duplicate) {
        link = -1;
        continue;
      }
      link = static_cast<std::int16_t>(resolved + 0x80);
    }
  }
  // n\x91bu nebula/region backdrops (g_system_region_trigger_defs fill loop
  // in NovaData_LoadScenarioResourceTables 0x004bd3c0: 32 slots, each probed
  // as id 0x80 + i; absent resources leave the rect zeroed so the starmap
  // skips them). Payload: 4 BE i16 rect fields, ActiveOn CString at +0x08,
  // OnExplore CString at +0x107.
  nebulae.assign(0x20, {});
  std::size_t loaded_nebulae = 0;
  for (std::int32_t id = 0x80; id <= 0x9f; ++id) {
    if (const auto res = NovaResource_Load(scenario::kNebulaResourceType,
                                           static_cast<std::uint16_t>(id))) {
      game::Nebula neb;
      neb.x = ReadBeI16(*res, 0x00);
      neb.y = ReadBeI16(*res, 0x02);
      neb.width = ReadBeI16(*res, 0x04);
      neb.height = ReadBeI16(*res, 0x06);
      neb.active_on_expression = ReadCString(*res, 0x08);
      neb.on_explore_expression = ReadCStringBounded(*res, 0x107, 0x100);
      nebulae[static_cast<std::size_t>(id) - 0x80] = std::move(neb);
      ++loaded_nebulae;
    }
  }
  for (std::int32_t id = 0x80; id <= 0x17f; ++id) {
    if (const auto res =
            NovaResource_LoadNamed(scenario::kGovernmentResourceType,
                                   static_cast<std::uint16_t>(id))) {
      game::Government gov = DecodeGovernment(res->bytes);
      // The record name is authoritative for the display name; the
      // target-code/comm/medium name tables come from the numeric payload
      // strings (DecodeGovernment).
      gov.name = res->name;
      governments[static_cast<std::size_t>(id) - 0x80] = std::move(gov);
      ++loaded_governments;
    }
  }
  // Random-encounter fleet defs (Nova Data 1 fl\x91t family). The original
  // walks ids up to 0x100 slots and zero-fills absent ones; a def with no lead
  // ship (< 0x80 lead) is suppressed by the DecodeFleet rebasing.
  for (std::int32_t id = 0x80; id <= 0x17f; ++id) {
    if (const auto res = NovaResource_LoadNamed(
            scenario::kFleetResourceType, static_cast<std::uint16_t>(id))) {
      fleets[static_cast<std::size_t>(id) - 0x80] = DecodeFleet(res->bytes);
      ++loaded_fleets;
    }
  }
  // Dude defs (Nova Data 1 d\x9fde family), up to 0x200 slots. Decoded after
  // the ship-class table so DecodeDude can validate each ship-type reference
  // against the loaded g_ship_class_defs (the loader's "nonexistent" marker
  // check).
  for (std::int32_t id = 0x80; id < 0x27f; ++id) {
    if (const auto res = NovaResource_LoadNamed(
            scenario::kDudeResourceType, static_cast<std::uint16_t>(id))) {
      dudes[static_cast<std::size_t>(id) - 0x80] =
          DecodeDude(res->bytes, ships);
      ++loaded_dudes;
    }
  }
  // Mission definitions (Nova Data m\x957n family). Preserve absent slots as
  // defaults, matching the original's 1000-entry zero-filled table. The
  // payload is retained in MissionDef while only loader-confirmed fields are
  // decoded in this first pass.
  for (std::int32_t id = 0x80; id < 0x80 + 1000; ++id) {
    if (const auto res = NovaResource_LoadNamed(
            scenario::kMissionResourceType, static_cast<std::uint16_t>(id))) {
      MissionDef mission = DecodeMission(res->bytes);
      mission.present = true;
      // Ghidra 0x0043bbb0 (misn loader) runs the resource name through
      // NameString_StripSubtitleSuffix (0x004cd230) before storing it, so the
      // BBS/computer list rows show "Base Name" for "Base Name;Subtitle".
      mission.display_name = StripSubtitleSuffix(res->name);
      missions[static_cast<std::size_t>(id) - 0x80] = std::move(mission);
      ++loaded_missions;
    }
  }
  // p\x91rs personalities (NovaData_LoadScenarioResourceTables personality
  // pass from 0x004c33de). Decoded after the ship-class table so ShipType
  // references can be validated against the -9999 tech-level sentinel.
  std::size_t loaded_pers = 0;
  for (std::int32_t id = 0x80; id < 0x80 + 0x400; ++id) {
    if (const auto res = NovaResource_LoadNamed(
            scenario::kPersResourceType, static_cast<std::uint16_t>(id))) {
      PersDef def = DecodePers(res->bytes, ships);
      // +0x624 display_name_buf: record name with the ';'-subtitle stripped
      // (NameString_StripSubtitleSuffix 0x004cd230).
      def.display_name = StripSubtitleSuffix(res->name);
      pers_defs[static_cast<std::size_t>(id) - 0x80] = std::move(def);
      ++loaded_pers;
    }
  }
  // crön events (loader cron pass, after the përs personalities). 0x200
  // slots, slot i = resource id 0x80 + i; absent resources keep !present,
  // matching the loader's 0xffff duration sentinel that the daily tick
  // skips on.
  std::size_t loaded_crons = 0;
  for (std::int32_t id = 0x80; id < 0x80 + 0x200; ++id) {
    if (const auto res = NovaResource_LoadNamed(
            scenario::kCronResourceType, static_cast<std::uint16_t>(id))) {
      cron_events[static_cast<std::size_t>(id) - 0x80] = DecodeCron(res->bytes);
      ++loaded_crons;
    }
  }
  // Display-name id post-pass (0x004c3e20): every slot starts with its own
  // index at +0x78a, then same-named slots adopt the first slot's id. The
  // original compares +0x625 name tails with a first-byte bound; exact-name
  // grouping is equivalent for well-formed names.
  {
    std::map<std::string, std::int16_t> first_id_by_name;
    for (std::size_t i = 0; i < pers_defs.size(); ++i) {
      pers_defs[i].display_name_string_id = static_cast<std::int16_t>(i);
    }
    for (auto &def : pers_defs) {
      if (def.display_name.empty()) {
        continue;
      }
      if (const auto found = first_id_by_name.find(def.display_name);
          found != first_id_by_name.end()) {
        def.display_name_string_id = found->second;
      } else {
        first_id_by_name.emplace(def.display_name, def.display_name_string_id);
      }
    }
  }
  // TODO(decomp(0x004bd3c0)) skipped: the Shareware Enforcer sentinel pass
  // (slot 0x3ff, from 0x004c3ae2) needs the real-world shareware day counter
  // (DAT_0059799e via FUN_004d4480: days since the stored first-run
  // timestamp). Slot 0x3ff stays inactive until that lands.
  // Asteroid-type rows (r\x9aid family), 16 ids 0x80..0x8f.
  for (std::int32_t id = 0x80; id < 0x90; ++id) {
    if (const auto res = NovaResource_LoadNamed(
            scenario::kAsteroidResourceType, static_cast<std::uint16_t>(id))) {
      asteroid_defs[static_cast<std::size_t>(id) - 0x80] =
          DecodeAsteroidType(res->bytes);
      ++loaded_asteroid_types;
    }
  }
  // Impact/explosion definitions (NovaData_LoadScenarioResourceTables around
  // 0x004c54c0): source +0x00 is the integer frame-rate scale, +0x02 the
  // impact sound slot, and +0x04 the sprite-set index. The runtime multiplies
  // the scale by 0.01 before Frame_UpdateImpactEffectSprites uses it against
  // elapsed milliseconds. Missing entries retain the original defaults.
  for (std::int32_t id = 0x80; id < 0xc0; ++id) {
    if (const auto res = NovaResource_Load(scenario::kImpactEffectResourceType,
                                           static_cast<std::uint16_t>(id))) {
      ImpactEffect &effect =
          impact_effects[static_cast<std::size_t>(id - 0x80)];
      effect.frame_rate_scale =
          static_cast<float>(ReadBeI16(*res, 0x00)) * 0.01F;
      effect.impact_sound_slot = ReadBeI16(*res, 0x02);
      effect.sprite_set_id = ReadBeI16(*res, 0x04);
      ++loaded_impact_effects;
    }
  }

  NovaLog::Info(
      "scenario tables loaded: {} ships, {} outfits, {} weapons, {} stellars, "
      "{} systems, {} nebulae, {} governments, {} fleet defs, {} dude defs, "
      "{} asteroid types, {} impact effects, {} missions, {} personalities, "
      "{} cron events",
      loaded_ships,
      loaded_outfits,
      loaded_weapons,
      loaded_stellars,
      loaded_systems,
      loaded_nebulae,
      loaded_governments,
      loaded_fleets,
      loaded_dudes,
      loaded_asteroid_types,
      loaded_impact_effects,
      loaded_missions,
      loaded_pers,
      loaded_crons);
  return loaded_ships > 0 && loaded_weapons > 0;
}

// ---------------------------------------------------------------------------
// Purchase-time derived cost/mass
// ---------------------------------------------------------------------------
// Ghidra Outfit_ComputeOutfitPurchasePrice (0x0046e910) and
// Outfit_ComputeOutfitPurchaseMass (0x0046e950): the outfit store computes
// these from the outfit's cost/mass with optional mass-proportional scaling.
// Flag 0x0200 (price) and 0x0400 (mass) make the value proportional to the ship
// class's hull mass; the scaled value never drops below the base positive
// value. Base cost > 0 is required for a price; base mass <= 0 yields no mass.
std::int32_t Outfit::PurchasePrice(std::int16_t ship_hull_mass) const {
  if (cost <= 0) {
    return 0;
  }
  std::int32_t scaled = cost;
  if ((flags & 0x0200U) != 0) {
    scaled = cost * ship_hull_mass;
    if (scaled < cost) {
      scaled = cost;
    }
  }
  return scaled;
}

std::int32_t Outfit::PurchaseMass(std::int16_t ship_hull_mass) const {
  if (mass_tons <= 0) {
    return mass_tons;
  }
  std::int32_t scaled = mass_tons;
  if ((flags & 0x0400U) != 0) {
    // ship Mass * hull Mass / 100 (the Bible); Ghidra uses a 0.01 float scale
    // (_DAT_00575738) and EVN-style round-half-away-from-zero.
    const float raw = static_cast<float>(mass_tons) * ship_hull_mass * 0.01F;
    scaled = static_cast<std::int32_t>(std::floor(raw + 0.5F));
    if (raw < 0) {
      scaled = static_cast<std::int32_t>(std::ceil(raw - 0.5F));
    }
    if (scaled < mass_tons) {
      scaled = mass_tons;
    }
  }
  return scaled;
}

// ---------------------------------------------------------------------------
// Nova control bit (NCB) test-expression evaluator
// ---------------------------------------------------------------------------
namespace {

// Recursive-descent evaluator over the Bible's NCB test-expression grammar:
//   expr   := or
//   or     := and ( '|' and )*
//   and    := unary ( '&' unary )*
//   unary  := '!' unary | primary
//   primary:= '(' expr ')' | '[' set ']' [cmp number] | token
//   set    := token*            (counts the number of 1-valued tokens)
//   token  := 'B'number | 'P'number | 'G' | 'O'number | 'E'number | '0' | '1'
class ExprParser {
public:
  ExprParser(std::string_view text, const ControlExpressionState &state)
      : text_(text), state_(state) {}

  [[nodiscard]] bool Eval() {
    const bool value = ParseOr();
    SkipWhitespace();
    return value;
  }

private:
  void SkipWhitespace() {
    while (pos_ < text_.size() && text_[pos_] == ' ') {
      ++pos_;
    }
  }

  [[nodiscard]] char Peek() const {
    return pos_ < text_.size() ? text_[pos_] : '\0';
  }

  void Consume() { ++pos_; }

  [[nodiscard]] std::int32_t ParseNumber() {
    std::int32_t value = 0;
    bool any = false;
    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
      value = value * 10 + (text_[pos_] - '0');
      ++pos_;
      any = true;
    }
    return any ? value : -1;
  }

  [[nodiscard]] bool ParseOr() {
    bool left = ParseAnd();
    for (;;) {
      SkipWhitespace();
      if (Peek() != '|') {
        return left;
      }
      Consume();
      const bool right = ParseAnd();
      left = left || right;
    }
  }

  [[nodiscard]] bool ParseAnd() {
    bool left = ParseUnary();
    for (;;) {
      SkipWhitespace();
      if (Peek() != '&') {
        return left;
      }
      Consume();
      const bool right = ParseUnary();
      left = left && right;
    }
  }

  [[nodiscard]] bool ParseUnary() {
    SkipWhitespace();
    if (Peek() == '!') {
      Consume();
      return !ParseUnary();
    }
    return ParsePrimary();
  }

  [[nodiscard]] bool ParsePrimary() {
    SkipWhitespace();
    const char c = Peek();
    if (c == '(') {
      Consume();
      const bool value = ParseOr();
      SkipWhitespace();
      if (Peek() == ')') {
        Consume();
      }
      return value;
    }
    if (c == '[') {
      return ParseCountedSet();
    }
    return ParseTokenTerm() != 0;
  }

  // Counted set: count the 1-valued tokens inside [ ], optionally compared to
  // a following number with '=' / '<' / '>'. Returns nonzero if the count
  // satisfies the comparison (or is nonzero when no comparison is given).
  [[nodiscard]] bool ParseCountedSet() {
    Consume(); // '['
    std::int32_t ones = 0;
    for (;;) {
      SkipWhitespace();
      const char c = Peek();
      if (c == '\0' || c == ']') {
        break;
      }
      if (c == '(') {
        ones += ParsePrimary() ? 1 : 0;
        continue;
      }
      if (ParseTokenTerm() != 0) {
        ++ones;
      }
    }
    SkipWhitespace();
    if (Peek() == ']') {
      Consume();
    }
    SkipWhitespace();
    const char op = Peek();
    if (op == '=' || op == '<' || op == '>') {
      Consume();
      SkipWhitespace();
      const std::int32_t rhs = ParseNumber();
      if (op == '=') {
        return ones == rhs;
      }
      return op == '<' ? ones < rhs : ones > rhs;
    }
    return ones != 0;
  }

  // Evaluates a single NCB term and returns 1 (true) or 0 (false). The token
  // letter is case-insensitive; numbers may be multi-digit.
  [[nodiscard]] int ParseTokenTerm() {
    SkipWhitespace();
    char c = Peek();
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      Consume();
    } else if (c == '0' || c == '1') {
      // Literal boolean: consume only the digit (a following digit would not
      // be a valid token start in Nova expressions).
      Consume();
      return c == '1' ? 1 : 0;
    } else {
      // Unknown token: advance one char and treat as false.
      if (c != '\0') {
        Consume();
      }
      return 0;
    }
    switch (static_cast<unsigned char>(c)) {
    case 'B':
    case 'b': {
      const std::int32_t bit = ParseNumber();
      if (bit >= 0 && state_.get_control_bit) {
        return state_.get_control_bit(static_cast<std::uint32_t>(bit)) ? 1 : 0;
      }
      return 0;
    }
    case 'P':
    case 'p': {
      const std::int32_t days = ParseNumber();
      if (state_.is_registered) {
        return state_.is_registered(days) ? 1 : 0;
      }
      return 0;
    }
    case 'G':
    case 'g': {
      return state_.is_male && state_.is_male() ? 1 : 0;
    }
    case 'O':
    case 'o': {
      const std::int32_t outfit_id = ParseNumber();
      if (state_.owns_outfit) {
        return state_.owns_outfit(static_cast<std::int16_t>(outfit_id)) ? 1 : 0;
      }
      return 0;
    }
    case 'E':
    case 'e': {
      const std::int32_t system_id = ParseNumber();
      if (state_.has_explored) {
        return state_.has_explored(static_cast<std::int16_t>(system_id)) ? 1
                                                                         : 0;
      }
      return 0;
    }
    default:
      return 0;
    }
  }

  std::string_view text_;
  const ControlExpressionState &state_;
  std::size_t pos_ = 0;
};

} // namespace

bool NovaControlExpression_Evaluate(std::string_view expression,
                                    const ControlExpressionState &state) {
  if (expression.empty()) {
    return true; // blank test expression evaluates to true
  }
  return ExprParser{expression, state}.Eval();
}

void NovaControlExpression_ExecuteSet(
    std::string_view expression, const ControlExpressionMutation &mutation) {
  if (!mutation.set_control_bit) {
    return;
  }
  // Set expressions are a stream of directives.  The landing-store scripts
  // observed in the scenario use B<number> to set a control bit; accepting an
  // explicit ! or =0 form also makes clearing state unambiguous.  Other
  // directive families are left untouched until their state targets are
  // reconstructed rather than being guessed as inventory mutations.
  for (std::size_t pos = 0; pos < expression.size();) {
    while (pos < expression.size() &&
           (expression[pos] == ' ' || expression[pos] == ',' ||
            expression[pos] == ';')) {
      ++pos;
    }
    bool value = true;
    if (pos < expression.size() && expression[pos] == '!') {
      value = false;
      ++pos;
    }
    if (pos >= expression.size() ||
        (expression[pos] != 'B' && expression[pos] != 'b')) {
      while (pos < expression.size() && expression[pos] != ',' &&
             expression[pos] != ';') {
        ++pos;
      }
      continue;
    }
    ++pos;
    const std::size_t number_start = pos;
    std::uint32_t bit = 0;
    while (pos < expression.size() && expression[pos] >= '0' &&
           expression[pos] <= '9') {
      bit = bit * 10U + static_cast<std::uint32_t>(expression[pos] - '0');
      ++pos;
    }
    if (pos == number_start) {
      continue;
    }
    if (pos < expression.size() && expression[pos] == '=') {
      ++pos;
      if (pos < expression.size() && expression[pos] == '0') {
        value = false;
        ++pos;
      } else if (pos < expression.size() && expression[pos] == '1') {
        value = true;
        ++pos;
      }
    }
    mutation.set_control_bit(bit, value);
  }
}

} // namespace game
