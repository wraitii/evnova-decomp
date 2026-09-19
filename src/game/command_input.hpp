#pragma once

// Process-wide access to the game's rebindable command table, mirroring the
// original's g_player_key_bindings +
// NovaInput_IsCommandActiveWithGameplayGuards (0x00469ca0). The table is owned
// by NovaRuntime::prefs and installed once at startup (NovaApp_Run); the Key
// Settings dialog edits it in place, so queries see rebinds immediately and
// nothing has to re-sync.
//
// A single process-wide pointer is deliberate, the same way the archive
// registry behind NovaResource_Load and the NovaLog sink are: the command table
// is a load-once, query-many service with no meaningful second instance. It is
// the one place that maps "gameplay command id -> current key", so the flight
// loop and the modal dialogs all resolve it identically instead of each reading
// KeyBindings::cmd_to_key by hand.
//
// Before Install is called (or after Install(nullptr)) queries resolve against
// NovaPrefs_ResetKeyBindings' defaults, so a test or early-startup poll still
// sees the documented default keys rather than nothing.

#include <cstddef>
#include <cstdint>

class SdlPlatform;

namespace game {

struct KeyBindings;

// Installs the authoritative binding table (nullptr reverts to the built-in
// defaults). Non-owning: the table must outlive every query. Intended to be
// called once at startup with `&runtime.prefs.bindings`.
void NovaInput_InstallCommandBindings(const KeyBindings *bindings);

// The key code bound to `command`, or 0xffff when the slot is out of range or
// unbound (both 0xff and 0xffff in the table mean unbound). Exposed separately
// from IsCommandActive so binding-driven display text can resolve a slot
// without a platform/key-state dependency.
[[nodiscard]] std::uint16_t NovaInput_CommandKey(std::size_t command);

// True when the key bound to `command` is currently held. Mirrors
// NovaInput_IsCommandActiveWithGameplayGuards minus the modal/panel-suppression
// guards the port does not model; false for an unbound slot.
[[nodiscard]] bool NovaInput_IsCommandActive(SdlPlatform &platform,
                                             std::size_t command);

} // namespace game
