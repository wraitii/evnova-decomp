#pragma once

#include "sdl_audio.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// BRGR container access is a reconstruction adapter, not a direct Ghidra
// function. It parses the container entry table (a 12-byte header plus a
// packed (offset, size, name) table) and the big-endian resource.map, then
// resolves (type, id) pairs to archive regions. This replaces the previous
// hardcoded region indices, which mis-assigned the odd sp\x95n resources
// (601/603/605/606) and the splash PICTs.

// Resource type codes as stored in resource.map records (big-endian FourCCs).
constexpr std::uint32_t kResourceTypeSprites = 0x7370956e; // "sp\x95n"
constexpr std::uint32_t kResourceTypeColors = 0x639a6c72;  // "c\x9alr"
constexpr std::uint32_t kResourceTypeRleSheet8 =
    0x726c9138; // "rl\x9138" (8-bit sheets)
constexpr std::uint32_t kResourceTypeRleSheet16 =
    0x726c9144; // "rl\x91D" (16-bit sheets)
constexpr std::uint32_t kResourceTypePict = 0x50494354; // "PICT"
constexpr std::uint32_t kResourceTypeSnd =
    0x736e6420; // "snd " (AIFF-style sounds)
// "DLOG" (0x444c4f47) / "DITL" (0x4449544c) live in the core EV Nova/Nova.rez
// UI archive. The dialog system builds every in-game window from a DLOG (which
// selects a DITL by id) plus that DITL's item rects/types/titles. The docked
// Spaceport window is DLOG/DITL 0x3e8 (618x517, matching PICT 0x2134 "Spaceport");
// the docked sub-windows are DITL 0x3e9 Trade, 0x3ea Outfit, 0x3ec Shipyard,
// 0x3ee Mission Select, 0x3f5 Bar, 0x3f6 News.
constexpr std::uint32_t kResourceTypeDialog = 0x444c4f47;
constexpr std::uint32_t kResourceTypeDialogItemList = 0x4449544c;
// Stellar "landing description" family ("desc", 0x64 0x91 0x73 0x63). Each
// stellar has a small block whose leading NUL-terminated C-string is the text
// the docked/Spaceport window shows about the place you are landing at
// (Ghidra NovaUi_RunTravelDestinationInteractionLoop, 0x00491f30, loads it via
// Ui_LoadSelectionDialogResource with id = the destination stellar resource
// id). The same family also holds sub-window flavour text at other id ranges
// (e.g. the Bar establishment description at stellar_id + 10000).
constexpr std::uint32_t kResourceTypeDescription = 0x64917363; // "d\x91sc"
constexpr std::uint32_t kResourceTypeMenu = 0x4d454e55; // "MENU"
constexpr std::uint32_t kResourceTypeAlert = 0x414c5254; // "ALRT"
constexpr std::uint32_t kResourceTypeControl = 0x434e544c; // "CNTL"
// "ch"♦r" (ch\x9ar) — the single default character/pilot-type resource.
// Same FourCC (0x63688a72) the game uses as the pilot-save registry key; the
// resource carries the new-pilot intro frame ids and per-frame delays (Nova
// Bible `ch♦r` IntroPict1-4 / PictDelay1-4). Only Nova Data 1.rez holds it.
constexpr std::uint32_t kResourceTypeCharacter = 0x63688a72;

// The new-pilot intro portion of the ch\x9ar character resource (Ghidra
// IntroCinematic_SetupFrames reads these same fields from the pilot-save
// block). Up to four PICT ids shown in sequence; the delay field is stored in
// 1/60s ticks and feeds IntroCinematicData::duration_60h_ticks directly (the
// intro timer waits ticks * 60 ms).
struct NovaCharacterIntro {
  std::array<std::int16_t, 4> pict_ids{-1, -1, -1, -1}; // IntroPict1-4
  std::array<std::int16_t, 4> delay_ticks{0, 0, 0, 0};  // PictDelay1-4
};

// Ghidra NovaData_LoadScenarioResourceTables loads the character resource
// (ch\x9ar, here the default .Trader id 0x0080) that defines a new pilot's
// intro cinematic. Returns nullopt when the archive/field set is absent.
[[nodiscard]] std::optional<NovaCharacterIntro>
NovaResource_LoadCharacterIntro();

// Ghidra: FUN_004ce250 + FUN_004cdfa0 (resource lookup by type + id). Returns
// the raw resource payload; the first archive holding a matching record wins,
// mirroring the game's archive search order.
[[nodiscard]] std::optional<std::vector<std::byte>>
NovaResource_Load(std::uint32_t type_code, std::uint16_t resource_id);

// Resource payload plus the record's display name from resource.map. The
// scenario loader (NovaData_LoadScenarioResourceTables, 0x004bd3c0) reads this
// name for ships/outfits/stellars/systems and uses it as the display name (the
// record-name string, not a field in the numeric payload).
struct NovaResource {
  std::vector<std::byte> bytes;
  std::string name;
};

[[nodiscard]] std::optional<NovaResource>
NovaResource_LoadNamed(std::uint32_t type_code, std::uint16_t resource_id);

// Ghidra: FUN_004ce2a0 + FUN_004ce030 (n-th record of a type, 1-based). The
// game reads the c\x9alr style through this accessor rather than by id.
[[nodiscard]] std::optional<std::vector<std::byte>>
NovaResource_LoadNthOfType(std::uint32_t type_code, std::size_t ordinal);

// Diagnostics: every distinct (type_code, resource_id) pair carried by the
// loaded BRGR resource maps, in archive order. Used to confirm which resource
// families the .rez containers expose (dialog/DLOG-DITL, PICT, sprites, ...).
std::vector<std::pair<std::uint32_t, std::uint16_t>> NovaResource_AllKeys();

// Resolves a file name inside the Nova Files data folder to an absolute path
// on disk (used for streaming assets such as background music), searching the
// known candidates for the game install. Returns nullopt if not found.
[[nodiscard]] std::optional<std::filesystem::path>
NovaResource_LocateFile(const std::string &file_name);

// On-disk layout documented as the "sp\x95n" resource in the Nova Bible.
// Field 0/1 are the sprite-graphics and mask resource ids (rl\x91D sheets for
// the menu buttons, PICT for the main-screen logo 606).
struct NovaSpriteDefinition {
  std::uint16_t sprites_resource_id = 0;
  std::uint16_t mask_resource_id = 0;
  std::uint16_t tile_width = 0;
  std::uint16_t tile_height = 0;
  std::uint16_t tiles_x = 0;
  std::uint16_t tiles_y = 0;
};

struct NovaRgbColor {
  std::uint8_t red = 0;
  std::uint8_t green = 0;
  std::uint8_t blue = 0;
};

struct NovaMenuPoint {
  std::int16_t x = 0;
  std::int16_t y = 0;
};

// The portions of c\x9alr used by the title screen. Its coordinate system is
// the original 1024x768 main-menu backdrop. Big-endian scalar fields.
struct NovaMainMenuStyle {
  NovaRgbColor menu_bright;
  NovaRgbColor menu_dim;
  std::uint16_t menu_font_size = 0;
  std::array<NovaMenuPoint, 6> button_origins{};
  // Ghidra: DAT_007d2524/26 loaded from c\x9alr +0xe0; the main-screen logo
  // (sp\x95n 606) anchor in the 1024x768 backdrop space.
  NovaMenuPoint logo_origin{};
  // Ghidra: c\x9alr +0xe4; sp\x95n 607 is the small center preview whose
  // frames correspond to the six menu actions plus one idle frame.
  NovaMenuPoint center_preview_origin{};
  // Ghidra: c\x9alr +0xe8/+0xec/+0xf0; sp\x95n 608-610 are the three
  // pre-rendered horizontal reveal strips behind the two button columns.
  std::array<NovaMenuPoint, 3> row_reveal_origins{};
};

[[nodiscard]] std::optional<NovaSpriteDefinition>
NovaSpriteDefinition_Parse(std::span<const std::byte> resource_data);

// Ghidra: FUN_004b4e10 resolves sp\x95n resources for FUN_004ad960's main-menu
// focus sprites 600-605 and main-screen logo 606.
[[nodiscard]] std::optional<NovaSpriteDefinition>
NovaResource_LoadMainMenuSpriteDefinition(std::uint16_t sprite_id);

[[nodiscard]] std::optional<NovaMainMenuStyle>
NovaMainMenuStyle_Parse(std::span<const std::byte> resource_data);

// Ghidra: NovaData_LoadScenarioResourceTables reads the first c\x9alr record
// (FUN_004ce2a0(0x639a6c72, 1)) and copies these fields into menu
// color/position globals.
[[nodiscard]] std::optional<NovaMainMenuStyle> NovaResource_LoadMainMenuStyle();

// Ghidra: 0x004ce250 resource acquisition beneath Resource_LoadPictAsImage.
// Covers the loading splash PICT 0x1fa4 and the startup splash PICT 0x83.
[[nodiscard]] std::optional<std::vector<std::byte>>
NovaResource_LoadSndData(std::uint16_t resource_id);

// Ghidra: 0x004d6e60 FUN_004d6e60 + 0x004d6900 FUN_004d6900. Decodes a Nova
// "snd " resource payload into device-agnostic PCM. The menu blips (ids
// 600/601) use the uncompressed 8-bit mono 'NONE' form: FUN_004d6d30 scans the
// format-2 rate table for a sub-structure offset, and the 8-bit sample data
// starts at that offset + 0x16. The game expands each 8-bit sample to 16-bit
// by (v + 0x80) | (v + 0x80) << 8 (a quirk preserved for fidelity).
//
// The classic header's 16.16 sample rate is preserved (600/601 use about
// 11127 Hz). The format-1 extended form used by snd 602/603 contains mono Apple
// IMA4 packets; those are decoded to PCM here because the original delegated
// them to its platform audio backend. Other AIFC forms remain unsupported.
[[nodiscard]] std::optional<NovaSoundData>
NovaSound_Decode(std::span<const std::byte> resource_data);

// Ghidra: FUN_004ce250 loads the "snd " (0x736e6420) resource family from the
// Nova Sounds.rez archive. Sound ids 600..603 are the intro/travel/loading
// transition/ambience sounds preloaded by NovaAudio_PreloadTransitionEffects
// (Ghidra 0x0048b250) into DAT_007d24b8; the menu hover/confirm blips live in
// the same family.
[[nodiscard]] std::optional<std::vector<std::byte>>
NovaResource_LoadPictData(std::uint16_t resource_id);

// Ghidra: title-screen backdrop PICT 0x1f40 in Nova Titles 1.rez. The
// original 1024x768 artwork shows a starship interior looking out at a
// planet; it is the backdrop behind the main-menu buttons (c\x9alr button
// origins share this 1024x768 coordinate space).
[[nodiscard]] std::optional<std::vector<std::byte>>
NovaResource_LoadMainMenuBackdropData();

// Ghidra: main-screen logo PICT 0x1f4a (sp\x95n 606, 654x209 tiles in a 7-frame
// vertical sheet). The animated "ESCAPE VELOCITY: NOVA" title drawn above the
// backdrop; its on-screen position comes from the c\x9alr offsets at +0xe0.
[[nodiscard]] std::optional<std::vector<std::byte>>
NovaResource_LoadMainMenuLogoData();

// One DITL item as decoded by the dialog parser FUN_004cef50. `rect` is the
// 4-coordinate item box in the dialog's own coordinate space; the ordering is
// (top, left, bottom, right) and the DLOG window is centred on screen before
// items are placed (FUN_008730a1). `type` is the low 7 bits of the type byte
// (bit 7 is the enabled/hilite flag). `enabled` mirrors that high bit.
struct NovaDialogItem {
  std::int16_t top = 0;
  std::int16_t left = 0;
  std::int16_t bottom = 0;
  std::int16_t right = 0;
  std::uint8_t type = 0;
  bool enabled = true;
};

// Ghidra: FUN_004cef50 (called by UiWindow_CreateFromDialogResource after it
// loads the DITL id referenced by a DLOG). Parses the DITL payload into its
// items, mirroring the game's byte arithmetic exactly: the big-endian item
// count at offset 0 (processed count+1 to include the trailing terminator),
// a per-item rect at +4..+11, the type byte at +12, then a variable tail that
// the walker advances past per item type (pascal-string items skip their
// title, icon/pict/control items skip a refcon short, plain items skip 14
// bytes), always re-aligning to an even offset. Returns nullopt when the
// resource is absent or a walk goes out of bounds.
[[nodiscard]] std::optional<std::vector<NovaDialogItem>>
NovaResource_LoadDialogItems(std::uint16_t dialog_item_list_id);

// A DLOG (dialog declaration) as read by Dialog_CreateFromDlog
// (FUN_008730a1). `bounds_*` is the window box in the DLOG's own coordinate
// space (its right-bottom minus left-top gives the window size);
// `dialog_item_list_id` is the DITL id linked at byte offset 18 that the
// dialog engine loads and parses for the window's items.
struct NovaDialogDefinition {
  std::int16_t top = 0;
  std::int16_t left = 0;
  std::int16_t bottom = 0;
  std::int16_t right = 0;
  std::uint16_t dialog_item_list_id = 0;
};

// Ghidra: FUN_004ce250(0x444c4f47) + Dialog_CreateFromDlog. Loads a DLOG
// (window declaration) resource: its BE bounds shorts at offsets 0..7 give the
// window size and the DITL id at offset 18. Returns nullopt when absent.
[[nodiscard]] std::optional<NovaDialogDefinition>
NovaResource_LoadDialogDefinition(std::uint16_t dialog_id);

// The decoded head of a stellar "desc" landing-description resource (Ghidra
// Ui_LoadSelectionDialogResource, 0x004c6d50). The leading C-string is the
// description text shown in the docked/Spaceport inner panel; the 2-byte BE
// `dialog_variant` and the trailing `status` line follow the text and feed the
// selection-dialog modals (not used by the docked landing panel).
struct NovaStellarDescription {
  std::string text;               // leading C-string: the landing description
  std::string status;             // trailing status C-string (<=0x20 chars)
  std::int16_t dialog_variant = 0; // 2-byte BE variant/picture id after text
};

// Ghidra: Ui_LoadSelectionDialogResource, called by
// NovaUi_RunTravelDestinationInteractionLoop (0x00491f30) with id = the
// destination stellar's raw resource id (>= 0x80). Loads that stellar's
// landing description and parses its leading text/status/variant. Returns
// nullopt when the archive/record is absent or the payload is too short.
[[nodiscard]] std::optional<NovaStellarDescription>
NovaResource_LoadStellarDescription(std::int16_t stellar_id);
