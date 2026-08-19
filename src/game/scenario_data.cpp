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
  w.range_scalar = std::bit_cast<float>(ReadBe32(bytes, 0x34));
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
  w.turret_arc_degrees = ReadBeI16(bytes, 0x30);
  w.shot_anim_frame_dwell = ReadBeI16(bytes, 0x32);
  w.kickback_impulse = ReadBeI16(bytes, 0x56);
  w.turret_group_id = ReadBeI16(bytes, 0x58);
  w.burst_cycle_ticks = ReadBeI16(bytes, 0x5a);
  w.burst_reset_cooldown = ReadBeI16(bytes, 0x5c);
  w.retarget_interval_ticks = ReadBeI16(bytes, 0x68);
  for (std::size_t i = 0; i < 4; ++i) {
    w.jam_vuln[i] = ReadBeI16(bytes, 0x5e + i * 2);
  }
  return w;
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
  s.armor_recharge = static_cast<float>(ReadBeI16(bytes, 0x36)) / 1000.0F;
  s.display_weight = ReadBeI16(bytes, 0x3c);
  s.mass_tons = ReadBeI16(bytes, 0x3e);
  s.length_meters = ReadBeI16(bytes, 0x40);
  s.default_ai_behavior = ReadBeI16(bytes, 0x42);
  s.timed_action_counter_init = ReadBeI16(bytes, 0x4c);
  s.crew = ReadBeI16(bytes, 0x44);
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
  // Ghidra NovaData_LoadScenarioResourceTables (0x004bd3c0) copies the
  // ionization capacity from ShipClassDef.ionization_capacity at payload
  // +0x36c. It is distinct from the nearby default-outfit count block.
  s.ionization_capacity = ReadBeI16(bytes, 0x36c);
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
  s.availability_expr = ReadCString(bytes, 0x6c);
  s.on_purchase_expr = ReadCString(bytes, 0x26a);
  s.on_retire_expr = ReadCString(bytes, 0x4cf);
  s.short_name = ReadCString(bytes, 0x5ce);
  s.long_name = ReadCString(bytes, 0x62e);
  s.buy_random = ReadBeI16(bytes, 0x388);
  s.hire_random = ReadBeI16(bytes, 0x38a);
  // Store masks (loader 0x004bd3c0): Contribute at shp +0x64/+0x68 and Require
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
  o.buy_random = ReadBeI16(bytes, 0x3f0);         // BuyRandom (1-100)
  o.item_class = ReadBeI16(bytes, 0x3f2);         // ItemClass
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
  st.government_id = ReadBeI16(bytes, 0x14); // Govt (resource id; <0x80 -> -1)
  if (st.government_id < 0x80) {
    st.government_id = -1;
  }
  st.min_status = ReadBeI16(bytes, 0x16);             // reputation_threshold
  st.engage_highlight_frame = ReadBeI16(bytes, 0x18); // hypergate pulse frame
  st.availability_flags = ReadBe16(bytes, 0x20);      // availability_flags
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
  return st;
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
//   name_table +0x44, scan_lo +0x54, scan_hi +0x58, jam2-4 +0x5c..+0x62,
//   medium_name +0x64, theme_color RGB24 +0xa4, ship_color RGB24 +0xa8,
//   interface_id +0xac, news_pic_id +0xae. The record name (resource.map, via
//   ResourceData_ReadEntryMetadata + StripSubtitleSuffix) is the display name.
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
  g.name = ReadCString(bytes, 0x44);
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
// SystemDef.murk at +0xbc; Ghidra previously mislabeled this field
// "alert_level" - the EV Nova Bible documents Murk as the starfield/ambience
// opacity, negative hides the starfield). The loader also reads DudeTypes/Prob
// (8 shorts each, with id rebasing/prob clamping), ReinfFleet/Time/Intrval near
// the end of the record, plus the Visibility string.
[[nodiscard]] System DecodeSystem(std::span<const std::byte> bytes) {
  System s;
  s.pos_x = ReadBeI16(bytes, 0x00);
  s.pos_y = ReadBeI16(bytes, 0x02);
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
  // Murk (s\xd8st +0x92): murkiness 0-100 (SystemDef.murk at +0xbc; Ghidra
  // previously mislabeled this field "alert_level"); a negative value
  // equivalently hides the starfield (NovaEffects_QueuedAmbientStarParticles
  // clears ambient stars when SystemDef.murk < 0).
  s.murk = ReadBeI16(bytes, 0x92);
  // AstTypes (s\xd8st +0x94): a 16-bit mask of allowed asteroid types. The
  // loader copies it verbatim into SystemDef.ast_types (+0x1f4);
  // Asteroid_Spawn (0x00421830) tests it via
  // (1 << (wander_type & 0x1f)) & ast_types.
  s.ast_types = static_cast<std::uint16_t>(ReadBeI16(bytes, 0x94));
  s.reinf_fleet = ReadBeI16(bytes, 0x196);
  s.reinf_time = ReadBeI16(bytes, 0x198);
  s.reinf_interval = ReadBeI16(bytes, 0x19a);
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

const FleetDef *ScenarioData::Fleet(std::int16_t resource_id) const {
  const auto index = static_cast<std::size_t>(resource_id) - 0x80;
  return index < fleets.size() ? &fleets[index] : nullptr;
}

const DudeDef *ScenarioData::Dude(std::int16_t resource_id) const {
  const auto index = static_cast<std::size_t>(resource_id) - 0x80;
  return index < dudes.size() ? &dudes[index] : nullptr;
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
      // The ship class display name is the resource record name (the loader
      // reads it via ResourceData_ReadEntryMetadata + StripSubtitleSuffix), not
      // a numeric header field.
      cls.display_name = res->name;
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
        if (const auto found = first_class_by_base_image.find(base_image);
            found != first_class_by_base_image.end()) {
          cls.clone_source_ship_class = found->second;
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
  for (std::int32_t id = 0x80; id <= 0x17f; ++id) {
    if (const auto res =
            NovaResource_LoadNamed(scenario::kGovernmentResourceType,
                                   static_cast<std::uint16_t>(id))) {
      game::Government gov = DecodeGovernment(res->bytes);
      // The record name is authoritative for the display name; comm/medium
      // name tables come from the numeric payload strings (DecodeGovernment).
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
      "{} systems, {} governments, {} fleet defs, {} dude defs, "
      "{} asteroid types, {} impact effects",
      loaded_ships,
      loaded_outfits,
      loaded_weapons,
      loaded_stellars,
      loaded_systems,
      loaded_governments,
      loaded_fleets,
      loaded_dudes,
      loaded_asteroid_types,
      loaded_impact_effects);
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
