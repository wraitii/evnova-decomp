#include "scenario_data.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
// All offsets verified against the Nova Data 4 payloads and the loader's
// weapon section (0x004bd3c0). Bible order: Reload0, Count2, MassDmg4,
// EnergyDmg6, Guidance8, Speed_a, AmmoType_c, Graphic_e, Inaccuracy10,
// Sound12, Impact14, ExplodType16, ProxRadius18, BlastRadius1a, Flags1c,
// Seeker1e, then SmokeSet20..PartColor2c (24-bit), BeamLength30, BeamWidth32,
// Falloff34, BeamColor36 (24-bit), CoronaColor3a (24-bit), the sub/particle
// block, and the jam/burst fields near 0x44+ / 0x56 / 0x6a / 0x72.
[[nodiscard]] Weapon DecodeWeapon(std::span<const std::byte> bytes) {
  Weapon w;
  w.reload_ticks = ReadBeI16(bytes, 0x00);
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
  w.impact = ReadBeI16(bytes, 0x14);
  w.explosion = ReadBeI16(bytes, 0x16);
  w.prox_radius = ReadBeI16(bytes, 0x18);
  w.blast_radius = ReadBeI16(bytes, 0x1a);
  w.flags = ReadBe16(bytes, 0x1c);
  w.seeker = ReadBe16(bytes, 0x1e);
  w.beam_length = ReadBeI16(bytes, 0x30);
  // Resource +0x32 is Ghidra WeaponDef.homing_strength_or_turn_rate (the
  // loader maps resource 0x32 -> +0x72); it is the shot animation frame-dwell
  // time in ms (see Weapon field comment in scenario_data.hpp).
  w.shot_anim_frame_dwell = ReadBeI16(bytes, 0x32);
  w.burst_count = ReadBeI16(bytes, 0x56);
  w.burst_reload = ReadBeI16(bytes, 0x58);
  w.max_ammo = ReadBeI16(bytes, 0x5a);
  for (std::size_t i = 0; i < 4; ++i) {
    w.jam_vuln[i] = ReadBeI16(bytes, 0x5e + i * 2);
  }
  w.flags2 = ReadBe16(bytes, 0x44);
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
  s.shield_recharge = static_cast<float>(ReadBeI16(bytes, 0x10));

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
  s.armor_recharge = static_cast<float>(ReadBeI16(bytes, 0x36));
  s.display_weight = ReadBeI16(bytes, 0x3c);
  s.mass_tons = ReadBeI16(bytes, 0x3e);
  s.length_meters = ReadBeI16(bytes, 0x40);
  s.default_ai_behavior = ReadBeI16(bytes, 0x42);
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
  s.flags_secondary = ReadBe16(bytes, 0x62);
  if (bytes.size() >= 0x728) {
    s.availability_flags = ReadBe16(bytes, 0x726);
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
  o.short_name = ReadCString(bytes, 0x32b);       // ShortName
  o.lc_name = ReadCString(bytes, 0x36b);          // LCName
  o.lc_plural = ReadCString(bytes, 0x3ab);        // LCPlural
  o.display_weight = ReadBeI16(bytes, 0x3ec);     // DispWeight
  o.sprite_id = ReadBeI16(bytes, 0x3ee);          // Graphic (p\x9ari sprite)
  o.buy_random = ReadBeI16(bytes, 0x3f0);         // BuyRandom (1-100)
  o.item_class = ReadBeI16(bytes, 0x3f2);         // ItemClass
  return o;
}

// ---------------------------------------------------------------------------
// sp\x9ab (Stellar / sp\xf6b) decode
// ---------------------------------------------------------------------------
// Offsets verified against Nova Data 2's stellar payloads and the loader's
// stellar section (0x004bd3c0): xPos+0, yPos+2, link_a_id+4 (the primary spin
// sprite-set id, id+1000 is the sp\x9an resource), travel_flags (32-bit)+6,
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
  st.flags = ReadBe32(bytes, 0x06);          // travel_flags
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
  // service_cost (payload +0x234; StellarDef +0x38), the landing/docking fee.
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
// DudeTypes+0x6e, % Prob+0x7e, govt+0x66, BkgndColor+0x8e (24-bit RRGGBB,
// Ghidra NovaData_LoadScenarioResourceTables reads a 32-bit at payload +0x8e
// and splits the three bytes into SystemDef.field_0x1ee/.f0/.f2), Murk+0x92
// (feeds SystemDef.murk at +0xbc; Ghidra previously mislabeled this field
// "alert_level" - the EV Nova Bible documents Murk as the starfield/ambience
// opacity, negative hides the starfield). The loader also reads DudeTypes/Prob
// (8 shorts each), a Message/Asteroids/Interference block and ReinfFleet/Time/
// Intrval near the end of the record, plus the Visibility string.
[[nodiscard]] System DecodeSystem(std::span<const std::byte> bytes) {
  System s;
  s.pos_x = ReadBeI16(bytes, 0x00);
  s.pos_y = ReadBeI16(bytes, 0x02);
  for (std::size_t i = 0; i < s.links.size(); ++i) {
    s.links[i] = ReadBeI16(bytes, 0x04 + i * 2);
    s.nav_defs[i] = ReadBeI16(bytes, 0x24 + i * 2);
  }
  s.government_id = ReadBeI16(bytes, 0x66);
  for (std::size_t i = 0; i < s.dude_types.size(); ++i) {
    s.dude_types[i] = ReadBeI16(bytes, 0x6e + i * 2);
    s.dude_prob[i] = ReadBeI16(bytes, 0x7e + i * 2);
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
  // TODO(decomp): AvgShips / Message / Asteroids / Interference / AstTypes
  // payload offsets are not yet confirmed against the loader; they stay
  // defaulted here. Interference and the background-color bit-repacking are
  // load-time transforms not yet reconstructed.
  s.reinf_fleet = ReadBeI16(bytes, 0x196);
  s.reinf_time = ReadBeI16(bytes, 0x198);
  s.reinf_interval = ReadBeI16(bytes, 0x19a);
  s.visibility_expr = ReadCString(bytes, 0x96);
  return s;
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

  std::size_t loaded_ships = 0;
  std::size_t loaded_weapons = 0;
  std::size_t loaded_outfits = 0;
  std::size_t loaded_stellars = 0;
  std::size_t loaded_systems = 0;
  std::size_t loaded_governments = 0;

  for (std::int32_t id = 0x80; id <= 0x27f; ++id) {
    if (const auto res = NovaResource_LoadNamed(
            scenario::kShipResourceType, static_cast<std::uint16_t>(id))) {
      ShipClass cls = DecodeShip(res->bytes);
      // The ship class display name is the resource record name (the loader
      // reads it via ResourceData_ReadEntryMetadata + StripSubtitleSuffix), not
      // a numeric header field.
      cls.display_name = res->name;
      ships[static_cast<std::size_t>(id) - 0x80] = std::move(cls);
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

  NovaLog::Info(
      "scenario tables loaded: {} ships, {} outfits, {} weapons, {} stellars, "
      "{} systems, {} governments",
      loaded_ships,
      loaded_outfits,
      loaded_weapons,
      loaded_stellars,
      loaded_systems,
      loaded_governments);
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

} // namespace game
