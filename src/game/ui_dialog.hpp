#pragma once

#include "../brgr_archive.hpp"
#include "../sdl_platform.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace game {

class NovaFontCache;

// SDL-backed port of the original's modal dialog runtime. Game-specific dialog
// code (e.g. Menu_RunPilotSelectionDialog 0x0048a7e0) runs on this layer near
// verbatim instead of on hard-coded geometry:
//
//   - UiWindow_CreateFromDialogResource 0x004cf760 (DLOG + DITL load, window
//     centring per Dialog_CreateFromDlog 0x008730a1, control-state setup)
//   - UiWindow_Draw 0x004d0d00 (white fill + black frame, per-type control
//     rendering with the bevel triples at 0x0056f118/11e/124/12a)
//   - UiWindow_RunInteractionLoop 0x004cfdd0 (per-frame event pump; reports
//     the 1-based ordinal of the activated control, or -1)
//   - the UiPanel_*/UiControl_* accessors used by every dialog call site
//
// Rows are 1-based DITL ordinals, exactly like the original's UiPanel calls.
struct UiDialogWindow {
  // One control's runtime state (the original's 0x28-byte control record:
  // type, value, selection range, payload pointer).
  struct ItemState {
    std::string text;                 // edit/static text content
    std::int32_t value = 0;           // checkbox 0/1, popup 1-based selection
    std::int32_t selection_start = 0; // edit-text selection (chars)
    std::int32_t selection_end = 0;
    std::string popup_title; // popup header (MENU title or runtime)
    std::vector<std::string> popup_entries; // popup entries (MENU or runtime)
    bool popup_expanded = false;
    // Type-0x40 image payload decoded lazily from the item's refcon PICT on
    // first draw.
    std::unique_ptr<SdlTexture> image;
  };

  NovaDialogDefinition definition;
  std::vector<NovaDialogItem> items;
  std::vector<ItemState> state;
  SDL_FRect window_rect{};     // centred on the 640x480 logical playfield
  std::size_t focused_row = 0; // 1-based row with keyboard focus (edit texts)

  [[nodiscard]] ItemState *Entry(std::size_t row_1based);
  [[nodiscard]] const ItemState *Entry(std::size_t row_1based) const;
  [[nodiscard]] const NovaDialogItem *Item(std::size_t row_1based) const;
};

// Ghidra 0x004cf760 UiWindow_CreateFromDialogResource. Loads the DLOG and its
// DITL through the BRGR archive, centres the window on the logical playfield
// (truncating half-offsets like Dialog_CreateFromDlog), and seeds control
// state; type-7 popups load their MENU resource entries when the DITL tail
// carries one. Returns nullopt when either resource is absent.
[[nodiscard]] std::optional<UiDialogWindow>
UiWindow_CreateFromDialogResource(SdlPlatform &platform,
                                  std::uint16_t dialog_id);

// Ghidra UiControl_SetValue / UiControl_GetValue.
void UiControl_SetValue(UiDialogWindow &window,
                        std::size_t row_1based,
                        std::int32_t value);
[[nodiscard]] std::int32_t UiControl_GetValue(const UiDialogWindow &window,
                                              std::size_t row_1based);

// Ghidra UiPanel_SetEntryTextPascal / UiPanel_GetEntryTextPascal.
void UiPanel_SetEntryTextPascal(UiDialogWindow &window,
                                std::size_t row_1based,
                                std::string_view text);
[[nodiscard]] std::string
UiPanel_GetEntryTextPascal(const UiDialogWindow &window,
                           std::size_t row_1based);

// Ghidra UiPanel_GetEntryTextPascalIndexed: the popup entry at the given
// 1-based control value. nullopt when the row is not a popup or the index is
// out of range.
[[nodiscard]] std::optional<std::string>
UiPanel_GetEntryTextPascalIndexed(const UiDialogWindow &window,
                                  std::size_t row_1based,
                                  std::int32_t value_1based);

// Ghidra UiPanel_SetTextEntrySelectionRange. An end of 0xfe means "to the
// end of the text" (the value every dialog call site uses for select-all).
void UiPanel_SetTextEntrySelectionRange(UiDialogWindow &window,
                                        std::size_t row_1based,
                                        std::int32_t start,
                                        std::int32_t end);

// Runtime fill of a type-7 popup's entries (the game fills MENU 0x1f5
// "Character" from the 0x63688a72 family this way; the shipped resource ships
// with the title only).
void UiPanel_SetEntryListItems(UiDialogWindow &window,
                               std::size_t row_1based,
                               std::string_view title,
                               std::vector<std::string> entries);

// Ghidra 0x004d0d00 UiWindow_Draw: window fill + frame, then one pass over the
// DITL items drawing each control from its parsed rect and runtime state.
// TODO(decomp(0x004d0d00)) skipped: type 0x20 gauges and 0x40 image blits draw
// as a plain plate because their image payloads (cicn refs in the control
// record) are not reconstructed.
void UiWindow_Draw(SdlPlatform &platform,
                   NovaFontCache &font_cache,
                   UiDialogWindow &window);

// Ghidra 0x004cfdd0 UiWindow_RunInteractionLoop: one frame of the modal loop.
// When `render_background` is set it is invoked first each frame so the screen
// under the dialog keeps rendering (the port draws the dialog straight onto
// the renderer; see the creation-site divergence note); otherwise the dialog
// composites over the last presented frame. Pumps text/mouse events, updates
// control state (edit-text focus/typing, checkbox toggles, popup
// expand/select), draws, and reports the 1-based ordinal of the activated
// control through *code_out (-1 when nothing activated this frame). Buttons
// activate on click; Enter activates the first enabled button and Escape the
// first button titled "Cancel"
// (TODO(decomp(0x004cfdd0)): the original's exact DIK-driven activation set is
// not reconstructed).
void UiWindow_RunInteractionLoop(
    SdlPlatform &platform,
    NovaFontCache &font_cache,
    UiDialogWindow &window,
    short *code_out,
    const std::function<void()> &render_background = {});

// Ghidra 0x004977d0 Ui_ShowConfirmDialog: the game's shared confirmation
// modal (DLOG 0xbba). Sets entry 3 to `message` and runs the interaction loop;
// OK (activation code 1) returns true, Cancel/dismiss (code 5) and quit return
// false. Returns false when the dialog resource is unavailable.
[[nodiscard]] bool
NovaUi_ShowConfirmDialog(SdlPlatform &platform,
                         NovaFontCache &font_cache,
                         std::string_view message,
                         const std::function<void()> &render_background = {});

// Ghidra 0x00497900 NovaUi_ShowTextConfirmCodeDialog: the shared text-entry
// modal (DLOG 0xbb9; row 3 = prompt, row 5 = edit text, activation code 1 =
// OK, 6 = Cancel). OK is accepted only when the text fits `max_chars`;
// otherwise the edit field is reselected and the loop continues. Returns the
// final text on accept, nullopt on cancel/quit or when DLOG 0xbb9 is missing.
// Call sites: new-game ship christening, shipyard purchase confirm, and the
// boarding-window captured-ship rename prompt.
[[nodiscard]] std::optional<std::string>
NovaUi_ShowTextEntryDialog(SdlPlatform &platform,
                           NovaFontCache &font_cache,
                           std::string_view prompt,
                           std::string_view initial_text,
                           std::int32_t max_chars,
                           const std::function<void()> &render_background = {});

// Maps one DITL item rect (top,left,bottom,right in the dialog's own space)
// to an SDL_FRect in playfield coordinates by adding the window origin.
[[nodiscard]] inline SDL_FRect ItemRect(const NovaDialogItem &item,
                                        const SDL_FRect &window) {
  return SDL_FRect{window.x + static_cast<float>(item.left),
                   window.y + static_cast<float>(item.top),
                   static_cast<float>(item.right - item.left),
                   static_cast<float>(item.bottom - item.top)};
}

} // namespace game
