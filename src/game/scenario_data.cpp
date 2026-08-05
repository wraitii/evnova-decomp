#include "scenario_data.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace game {
namespace {

[[nodiscard]] std::uint16_t ReadBe16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  if (offset + 2 > bytes.size()) {
    return 0;
  }
  return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])
                                    << 8U |
                                    std::to_integer<std::uint8_t>(
                                        bytes[offset + 1]));
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
  return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset])
                                    << 24U |
                                    std::to_integer<std::uint8_t>(
                                        bytes[offset + 1]) << 16U |
                                    std::to_integer<std::uint8_t>(
                                        bytes[offset + 2]) << 8U |
                                    std::to_integer<std::uint8_t>(
                                        bytes[offset + 3]));
}

[[nodiscard]] std::int32_t ReadBeI32(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  return static_cast<std::int32_t>(ReadBe32(bytes, offset));
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
  w.beam_width = ReadBeI16(bytes, 0x32);
  w.burst_count = ReadBeI16(bytes, 0x56);
  w.burst_reload = ReadBeI16(bytes, 0x58);
  w.max_ammo = ReadBeI16(bytes, 0x5a);
  w.guided_turn = ReadBeI16(bytes, 0x72);
  for (std::size_t i = 0; i < 4; ++i) {
    w.jam_vuln[i] = ReadBeI16(bytes, 0x6a + i * 2);
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
// o\x9ftf (Outfit), sp\x9ab (Stellar), s\xd8st (System) decode
// ---------------------------------------------------------------------------
// The binary layouts of these three are not yet fully verified against every
// payload, so the decoders below are provisional: they fill the primary fields
// that are positively identified (the outfit mod block and the system/stellar
// header fields) and leave the rest as defaults. They exist so the tables can
// be sized and indexed identically to the originals and grow as each field's
// offset is confirmed.
[[nodiscard]] Outfit DecodeOutfit(std::span<const std::byte> bytes) {
  Outfit o;
  o.display_weight = ReadBeI16(bytes, 0x00);
  o.mass_tons = ReadBeI16(bytes, 0x02);
  o.tech_level = ReadBeI16(bytes, 0x04);
  o.mod_type = ReadBeI16(bytes, 0x06);
  o.mod_val = ReadBeI16(bytes, 0x08);
  o.alt_mod_types[0] = ReadBeI16(bytes, 0x12);
  o.alt_mod_vals[0] = ReadBeI16(bytes, 0x14);
  o.alt_mod_types[1] = ReadBeI16(bytes, 0x16);
  o.alt_mod_vals[1] = ReadBeI16(bytes, 0x18);
  o.alt_mod_types[2] = ReadBeI16(bytes, 0x1a);
  o.alt_mod_vals[2] = ReadBeI16(bytes, 0x1c);
  o.max_count = ReadBeI16(bytes, 0x22);
  o.flags = ReadBe16(bytes, 0x24);
  o.cost = ReadBeI32(bytes, 0x26);
  return o;
}

[[nodiscard]] Stellar DecodeStellar(std::span<const std::byte> bytes) {
  Stellar st;
  st.pos_x = ReadBeI16(bytes, 0x00);
  st.pos_y = ReadBeI16(bytes, 0x02);
  st.graphic_type = ReadBeI16(bytes, 0x04);
  st.flags = ReadBe32(bytes, 0x06);
  st.tribute = ReadBeI16(bytes, 0x0a);
  st.tech_level = ReadBeI16(bytes, 0x0e);
  for (std::size_t i = 0; i < 8; ++i) {
    st.special_tech[i] = ReadBeI16(bytes, 0x10 + i * 2);
  }
  st.government_id = ReadBeI16(bytes, 0x24);
  return st;
}

[[nodiscard]] System DecodeSystem(std::span<const std::byte> bytes) {
  System s;
  s.pos_x = ReadBeI16(bytes, 0x00);
  s.pos_y = ReadBeI16(bytes, 0x02);
  for (std::size_t i = 0; i < 16; ++i) {
    s.links[i] = ReadBeI16(bytes, 0x04 + i * 2);
    s.nav_defs[i] = ReadBeI16(bytes, 0x24 + i * 2);
  }
  s.government_id = ReadBeI16(bytes, 0x66);
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

bool ScenarioData::LoadFromArchives() {
  // The original sizes these tables to the family maximum and zero-fills
  // missing slots (0x200 ships/outfits, 0x100 weapons). We mirror that so
  // callers can index directly by id - 0x80.
  ships.assign(0x200, {});
  outfits.assign(0x200, {});
  weapons.assign(0x100, {});
  stellars.assign(0x600, {});
  systems.assign(0x800, {});

  std::size_t loaded_ships = 0;
  std::size_t loaded_weapons = 0;
  std::size_t loaded_outfits = 0;
  std::size_t loaded_stellars = 0;
  std::size_t loaded_systems = 0;

  for (std::int32_t id = 0x80; id <= 0x27f; ++id) {
    if (const auto data = NovaResource_Load(scenario::kShipResourceType,
                                            static_cast<std::uint16_t>(id))) {
      ships[static_cast<std::size_t>(id) - 0x80] = DecodeShip(*data);
      ++loaded_ships;
    }
  }
  for (std::int32_t id = 0x80; id <= 0x27f; ++id) {
    if (const auto data = NovaResource_Load(scenario::kOutfitResourceType,
                                            static_cast<std::uint16_t>(id))) {
      outfits[static_cast<std::size_t>(id) - 0x80] = DecodeOutfit(*data);
      ++loaded_outfits;
    }
  }
  for (std::int32_t id = 0x80; id <= 0x17f; ++id) {
    if (const auto data = NovaResource_Load(scenario::kWeaponResourceType,
                                            static_cast<std::uint16_t>(id))) {
      weapons[static_cast<std::size_t>(id) - 0x80] = DecodeWeapon(*data);
      ++loaded_weapons;
    }
  }
  for (std::int32_t id = 0x80; id <= 0x57f; ++id) {
    if (const auto data = NovaResource_Load(scenario::kStellarResourceType,
                                            static_cast<std::uint16_t>(id))) {
      stellars[static_cast<std::size_t>(id) - 0x80] = DecodeStellar(*data);
      ++loaded_stellars;
    }
  }
  for (std::int32_t id = 0x80; id <= 0x47f; ++id) {
    if (const auto data = NovaResource_Load(scenario::kSystemResourceType,
                                            static_cast<std::uint16_t>(id))) {
      systems[static_cast<std::size_t>(id) - 0x80] = DecodeSystem(*data);
      ++loaded_systems;
    }
  }

  NovaLog::Info(
      "scenario tables loaded: {} ships, {} outfits, {} weapons, {} stellars, "
      "{} systems",
      loaded_ships, loaded_outfits, loaded_weapons, loaded_stellars,
      loaded_systems);
  return loaded_ships > 0 && loaded_weapons > 0;
}

} // namespace game
