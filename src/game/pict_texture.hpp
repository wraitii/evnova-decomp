#pragma once

// Loads one PICT resource into a texture (null on failure to locate or
// decode). Defined once in docked_dialog.cpp; shared by every window that
// draws a PICT backdrop or portrait.

#include <cstdint>
#include <memory>

class SdlPlatform;
class SdlTexture;

namespace game {

[[nodiscard]] std::unique_ptr<SdlTexture>
LoadPictTexture(SdlPlatform &platform, std::uint16_t pict_id);

} // namespace game
