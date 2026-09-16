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
// "ppat" pixel-pattern family: the radar interference static is one of the
// ten patterns 128..137 (see pixpat_image.hpp).
constexpr std::uint32_t kResourceTypePpat = 0x70706174; // "ppat"
constexpr std::uint32_t kResourceTypeCicn =
    0x6369636e; // "cicn" (color icons, e.g. the target reticles)
constexpr std::uint32_t kResourceTypeSnd =
    0x736e6420; // "snd " (AIFF-style sounds)
// "DLOG" (0x444c4f47) / "DITL" (0x4449544c) live in the core EV Nova/Nova.rez
// UI archive. The dialog system builds every in-game window from a DLOG (which
// selects a DITL by id) plus that DITL's item rects/types/titles. The docked
// Spaceport window is DLOG/DITL 0x3e8 (618x517, matching PICT 0x2134
// "Spaceport"); the docked sub-windows are DITL 0x3e9 Trade, 0x3ea Outfit,
// 0x3ec Shipyard, 0x3ee Mission Select, 0x3f5 Bar, 0x3f6 News.
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
constexpr std::uint32_t kResourceTypeMenu = 0x4d454e55;        // "MENU"
constexpr std::uint32_t kResourceTypeAlert = 0x414c5254;       // "ALRT"
constexpr std::uint32_t kResourceTypeControl = 0x434e544c;     // "CNTL"
// "STR#" string-list family (0x53545223): a BE u16 count followed by that
// many Pascal strings. The Settings dialog's sound-volume words are STR# 0x88
// (9 entries, indexed by volume+1).
constexpr std::uint32_t kResourceTypeStringTable = 0x53545223;
// "STR " single-string family: one Pascal string (u8 length + bytes). Nova
// uses these as sparse, plugin-friendly overrides for shared STR# entries.
constexpr std::uint32_t kResourceTypeString = 0x53545220;
// "ch"♦r" (ch\x9ar) — the single default character/pilot-type resource.
// Same FourCC (0x63688a72) the game uses as the pilot-save registry key; the
// resource carries the new-pilot intro frame ids and per-frame delays (Nova
// Bible `ch♦r` IntroPict1-4 / PictDelay1-4). Only Nova Data 1.rez holds it.
constexpr std::uint32_t kResourceTypeCharacter = 0x63688a72;

// A MENU resource as rendered by the stock dialog popup controls: a 0x10-byte
// classic menu header (id/proc/width/height/enable flags), a Pascal title, then
// entries of [Pascal string][u16 cmd][u16 glyph] terminated by a trailing 0x00.
// The type-7 DITL controls reference these by id (NovaDialogItem::
// menu_resource_id); popup entries may also be filled at runtime (the new-pilot
// dialog's Character popup, MENU 0x1f5, ships empty and gets the 0x63688a72
// family census). TODO(decomp): confirm the cmd/glyph u16 fields.
struct NovaMenuDefinition {
  std::string title;
  std::vector<std::string> entries;
};

// Loads and parses a MENU resource. Returns nullopt when absent or too short.
[[nodiscard]] std::optional<NovaMenuDefinition>
NovaResource_LoadMenuDefinition(std::uint16_t menu_id);

// Ghidra NovaData_LoadScenarioResourceTables loads the character resource
// (ch\x9ar, here the default .Trader id 0x0080) that defines a new pilot's
// intro cinematic. Returns nullopt when the archive/field set is absent.

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

// Ghidra 0x004ce300 ResourceData_AccessByKey restricted to the pilot-save
// family (0x63688a72, key = the registered metadata name): returns the first
// family entry whose registered name matches `key`, as ResourceData_FindByKey
// would, with the block materialized for reads. The primary keyed accessor
// used with the pilot-save id across the new-game flow (Menu_RunNewGameFlow
// keys the block by the dialog's selected character template name, falling
// back to family entry 1 when the 0xc1e dialog variant left it empty).
// The original also merges in-memory pilot blocks created during the session
// into the registry; the reimplementation has no in-session registry, so
// session-created pilots are invisible to the lookup.
// TODO(decomp) once a .plt writer feeds the registry.
[[nodiscard]] std::optional<NovaResource>
NovaResource_AccessCharacterBlockByKey(std::string_view key);

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
  // Ghidra: c\x9alr +0x56 -> DAT_007d827c and +0x5a -> DAT_007d8282, the two
  // colors the store grid uses (Bible GridBright = selection square,
  // GridDim = grid color). NovaUi_RedrawOutfitterMenu (0x00490c70) and
  // NovaUi_DrawShipyardShipList (0x004948b0) frame every page cell with
  // grid_dim and re-frame the selected cell with grid_bright.
  NovaRgbColor grid_bright;
  NovaRgbColor grid_dim;
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
  // The list/map palette the interface reads from the same c\x9alr
  // (DAT_00735658/5e/64/6a/70). NovaUi_DrawListRowCallback (0x00448a30)
  // fills rows with list_background/list_hilite and draws list_text; the
  // target category panel uses escort_hilite and the route map floating_map.
  NovaRgbColor floating_map;    // +0x8a
  NovaRgbColor list_text;       // +0x8e
  NovaRgbColor list_background; // +0x92
  NovaRgbColor list_hilite;     // +0x96
  NovaRgbColor escort_hilite;   // +0x9a
  // Ghidra: c\x9alr +0x5e..+0x64, the startup loading-progress-bar outline in
  // native QuickDraw (top,left,bottom,right) order and relative to the window
  // center (Bible c\x9alr ProgressBar). NovaUi_RunProgressBarReveal
  // (0x004ab1b0) offsets it by the render-owner center (0x004c69ab seeds it)
  // and NovaUi_RedrawProgressBar (0x004ab3d0) fills it from
  // g_loading_progress_value/total over DAT_00575a58 = 198.0 reference px.
  std::int16_t progress_bar_top = 280;
  std::int16_t progress_bar_left = -100;
  std::int16_t progress_bar_bottom = 290;
  std::int16_t progress_bar_right = 100;
  // Bible c\x9alr ProgBright/ProgDim/ProgOutline. Ghidra 0x004c6625/
  // 0x004c6667/0x004c66a9 copy them into the bar colour globals.
  NovaRgbColor progress_fill;  // +0x66 (bright fill)
  NovaRgbColor progress_inner; // +0x6a (fill outline)
  NovaRgbColor progress_outer; // +0x6e (bar outline)
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
// 11127 Hz). Two format-1 sub-forms are handled: an extended header containing
// mono Apple IMA4 packets (snd 602/603 and the IMA4 weapon sounds), and the
// 'NONE' 8-bit mono payload that backs most weapon fire sounds (ids 200..235).
// Both are decoded to PCM here because the original delegated the codec
// conversion to its platform audio backend. Other AIFC forms remain
// unsupported.
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
  // Zero-based DITL item index.  Dialog code addresses controls by this
  // ordinal (rather than by their screen geometry), so callers must retain it
  // even when several controls happen to share the same size.
  std::size_t index = 0;
  std::int16_t top = 0;
  std::int16_t left = 0;
  std::int16_t bottom = 0;
  std::int16_t right = 0;
  std::uint8_t type = 0;
  // Bit 7 of the raw type byte. NOT an enable/draw gate: UiWindow_Draw draws
  // every item whose flag byte is nonzero and ignores this bit (DITL 0xc1d
  // ships all interactive controls with bit 7 clear and they render in the
  // real game). Kept for resource fidelity only.
  bool enabled = true;
  // The item's Pascal-string caption, captured for text-like item types
  // (4 button, 5 checkbox, 6 radio, 8 static, 0x10 edit). Checkbox/static
  // items carry their on-screen label here (e.g. the preferences dialog's
  // "Ship Animations"); plain/control items leave it empty. Stored as the
  // raw resource bytes (no NUL padding).
  std::string title;
  // Types 7/0x20/0x40 only: the tail's subtype byte and the BE u16 refcon.
  // The engine resolves the refcon per subtype: type-7 popups load a MENU
  // resource (DITL 0xc1d item 10 -> MENU 0x1f4 "Gender", item 12 -> MENU
  // 0x1f5 "Character" whose entries are filled at runtime), 0x40 image items
  // blit a PICT (DITL 0xc1d items 2/13 -> PICT 129/130).
  std::uint8_t popup_subtype = 0;
  std::uint16_t refcon = 0;
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
  std::string text;                // leading C-string: the landing description
  std::string status;              // trailing status C-string (<=0x20 chars)
  std::int16_t dialog_variant = 0; // 2-byte BE variant/picture id after text
};

// Ghidra: Ui_LoadSelectionDialogResource, called by
// NovaUi_RunTravelDestinationInteractionLoop (0x00491f30) with id = the
// destination stellar's raw resource id (>= 0x80). Loads that stellar's
// landing description and parses its leading text/status/variant. Returns
// nullopt when the archive/record is absent or the payload is too short.
[[nodiscard]] std::optional<NovaStellarDescription>
NovaResource_LoadStellarDescription(std::int16_t stellar_id);

// Reads an arbitrary `desc` resource through the selection-dialog format.
// Landed stores use outfit_id + 3000 for selected-item descriptions.
[[nodiscard]] std::optional<NovaStellarDescription>
NovaResource_LoadDescription(std::uint16_t resource_id);
