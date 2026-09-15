#pragma once

namespace game {

// Clean-room fixes for confirmed bugs in the original executable or shipped
// scenario data. Keep every use marked `BUGFIX(original)` so this constant can
// later be replaced by a user preference without conflating other deliberate
// SDL-port divergences with gameplay corrections.
inline constexpr bool kApplyOriginalBugFixes = true;

} // namespace game
