#pragma once

// Loads one PICT resource into a texture (null on failure to locate or
// decode). Defined once in docked_dialog.cpp; shared by every window that
// draws a PICT backdrop or portrait.

#include <cstdint>
#include <memory>

class SdlPlatform;
class SdlTexture;
struct SDL_Texture;

namespace game {

[[nodiscard]] std::unique_ptr<SdlTexture>
LoadPictTexture(SdlPlatform &platform, std::uint16_t pict_id);

// Draw native PICT dimensions centred in the window, containing only when
// needed. Restores the caller's placement after drawing.
void DrawContainedPict(SdlPlatform &platform, SDL_Texture *texture);

} // namespace game
