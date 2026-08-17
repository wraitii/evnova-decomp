# Menu Graphics Structure (for SDL3 reimplementation)

How EV Nova loads and renders the sprites/images for the **main menu / intro / splash**
path, independent of the in-flight gameplay graphics. The graphics internals
(RLE sprite decoding, blitters) will be replaced by SDL3, but this is the call
structure to mimic.

## The two rendering models

EV Nova uses two distinct graphics systems:

1. **Sprite / Image objects** — loaded from `.rez` resources, drawn onto surfaces via
   a DrawContext (a "current draw target" abstraction).
2. **SpriteWorld** — a layered scene/dirty-rect engine used for the in-space flight loop.
   *Not part of the menu path.* Relevant menu rendering uses model 1 only.

The menu/splash/intro path uses a **shared offscreen compositing surface**:
- `DAT_00597950` — the offscreen sprite/panel DrawContext handle (full-size gameplay
  surface, allocated by `FUN_004ac950` via `DrawContext_AllocateFromPictResource (0x0046f740)`).
- `DAT_00597954` — the full-game-size rect bounding that surface.
- Everything menu/splash is drawn into `DAT_00597950`, then that surface is blitted to
  the main **render owner** (the OS window surface), then committed.

## Loading

### Plain images (splash / intro backdrops) — PICT resources
- `Resource_LoadPictAsImage(id)` → `Resource_LoadPictAsImageWithColorRemap(id, remap)`
  - Reads a PICT-type `.rez` resource, allocates an image/surface wrapper, decodes it.
- `Image_Destroy(img)` — frees the wrapper (splash/intro images are transient).
- Splash/intro code pattern:
  ```
  img = Resource_LoadPictAsImage(id)
  DrawContext_PushCurrentAndSet(&DAT_00597950)      // target = offscreen surface
  Rect_CenterImageInRect(img, &rect)                // center into DAT_00597954
  DrawContext_BlitImageToRect(img, &rect)           // draw image into offscreen
  DrawContext_RestorePreviousCurrent()
  ... blit offscreen -> render owner, NovaRender_CommitFrame()
  Image_Destroy(img)
  ```
  Seen in `NovaUi_PresentStartupSplashFrame` (PICT 0x83), `NovaUi_PresentLoadingSplashFrame`
  (PICT 0x1fa4), and the new-game intro cinematic `IntroCinematic_Run` (frames from pilot
  data; NOT the boot splash).

### Animated sprites — RLE sprite sheets (.rez)
- `FUN_004b4e10(id, ...)` — reads the sprite-set resource header (frame count, dims,
  anchors). Returns valid-resource flag.
- `Resource_IsResourceTypePresent(type, id) (0x0047b7a0)` — checks whether the resource family
  (FourCC type / id) exists in the archive DB, so the loader picks the RLE sprite-sheet form
  vs the multi-frame form.
- `Sprite_CreateFromSpriteSheetResources(...)` — RLE sprite-sheet form.
- `Sprite_CreateFromMultiFrameResource(...)` — multi-frame form.
- `Sprite_PrepareFramesAndAttachResourceData(sprite)` — build usable frames.
- `Sprite_CompileFramesAndDiscardSourceData(sprite)` — bake + discard source pixels.
- `Sprite_Create(handle, buf, frame_capacity)` / `Sprite_InitOrAllocate` / `Sprite_Clone`
  — the raw object allocators (Sprite.c).
- `Sprite_Release` / `Sprite_Destroy` — teardown.

### Menu sprite loading site
- `FUN_004ad960` (called from `NovaGameSession_Run` before the main loop) is the sprite
  table loader. It runs five loops, each a `FUN_004b4e10` header lookup + a create/prepare/
  compile sequence:
  - **Weapon sprite sets** (resource ids 3000+, count 0x100) → `g_weapon_sprite_set_table`
  - **Ship-set sprites** (ids 400+, count 0x40) → table `DAT_00596700`
  - ids 500+ (count 5) and ids 800+ (count 0x10) → similar tables
  - **6 menu focus sprites** (ids 600–605) → `DAT_00596cb8[6]`, each set to frame 0. This
    is the array used by the menu renderer and hover hit-test.
- `FUN_004aeda0` (from `NovaGameSession_Run`) loads per-ship-class visuals into
  `g_ship_class_defs` (via `ShipClass_LoadShipClassVisualAndLaunchData`).

## Rendering (per menu frame)

`NovaRender_RedrawAndPresentFrame(mode)` (0x004873b0) is the central frame renderer for
both menu and in-game HUD. Menu path:

1. Push `DAT_00597950` (offscreen sprite surface) as the draw target.
2. Menu branch (`DAT_00596d28 == 0`, no game active): draw the pulsing centered prompt
   string; if a game is loaded, draw the in-flight HUD status panel instead.
3. **Blit the 6 menu focus sprites** (`DAT_00596cb8[6]`):
   - `Sprite_SetPositionFromCurrentFrameAnchor(sprite, x, y)` — position each at
     `DAT_007d24cc[i] + DAT_007d2544`, `DAT_007d24ce[i] + DAT_007d2546`.
   - `BlitPixie_BlitRectRleCommandStream (0x00470ee0)` — the sprite→surface blit entry (`BlitPixieInterface.c`). Validates
     the surface/sprite, then calls the RLE command-stream renderers
     (`SpriteRleCommandStream_DecodeUnclipped (0x00471e10)` unscaled / `SpriteRleCommandStream_DecodeClippedRow (0x00471e90)` scaled+clipped). This is the function to
     replace with an SDL3 `SDL_RenderTexture`/blit.
4. `NovaHud_RenderOverlays()` — HUD transient/effect overlays.
5. `NovaHud_RenderFocusOverlay()` — indexed focus/animation overlay.
6. Restore owner context, blit offscreen surface to render owner.
7. `mode` 1 → `NovaRender_CommitFrame()` → `NovaRender_QueuePresentAndSwap()` (present).

### Menu hover / button interaction
- `NovaHud_TrackFocusHoverIndex()` (0x004861b0): positions the 6 focus sprites, then
  for each visible one calls `Sprite_TestOpaquePixelAtPoint(sprite, mouse)` to find which
  menu button region the cursor is over. Returns the hovered index (−1 = none) which the
  main loop passes to `NovaGameMode_DispatchAction(action)`.
- The 6 focus sprites double as both the hover highlight and the hit-test geometry.

### DrawContext (draw target) model
- `DrawContext_SetCurrent(ctx)` / `DrawContext_GetCurrent(out)` — process-global current
  target; most draw helpers implicitly render into it.
- `DrawContext_PushCurrentAndSet(&handle)` / `DrawContext_RestoreOwnerContext(owner)` —
  the save/switch pattern used to compose into offscreen surfaces then copy out.
- `DrawContext_BlitClippedRect` — the generic blit; `DrawContext_BlitImageToRect` wraps it
  for images.
- `DrawContext_SetRgbColor`, `DrawContext_SetFillRgbColor`, `DrawContext_SetFontId`,
  `DrawContext_DrawPascalString`, `DrawContext_FillRect16WithCurrentColor` — text/fill
  primitives (SDL3 text/rects).

## Suggested SDL3 mapping

| Legacy subsystem | SDL3 replacement |
|------------------|------------------|
| `Resource_LoadPictAsImage` / `Resource_LoadPictAsImageWithColorRemap` | `IMG_Load` → `SDL_Texture` cache |
| `Image_Destroy` | `SDL_DestroyTexture` |
| `Sprite*` / sprite tables (`DAT_00596cb8[6]`, `g_weapon_sprite_set_table`) | load animated sprite sheets into `SDL_Texture` arrays (or `SDL_RenderGeometry` frames) |
| `BlitPixie_BlitRectRleCommandStream (0x00470ee0)` (RLE blit) | `SDL_RenderCopyEx` / `SDL_RenderTexture` per frame |
| Offscreen surface `DAT_00597950` + rect `DAT_00597954` | an SDL target texture / `SDL_SetRenderTarget` for compositing |
| Render owner (OS window surface) | the SDL window renderer/present target |
| `NovaRender_CommitFrame` → `QueuePresentAndSwap` | `SDL_RenderPresent` |
| `DrawContext_SetCurrent` | switching the active compositing target |
| `NovaRender_RedrawAndPresentFrame` | the menu/scene render function, called once per frame |
| `Sprite_TestOpaquePixelAtPoint` (hover hit-test) | replace with rect/alpha-channel hit-test in item-space |

## Key symbols (menu/intro/splash render path)

| Addr | Role |
|------|------|
| `0x004873b0` `NovaRender_RedrawAndPresentFrame` | central menu/in-game frame renderer |
| `0x004861b0` `NovaHud_TrackFocusHoverIndex` | menu focus sprite hover hit-test |
| `0x0048c3c0` `NovaHud_RenderOverlays` | HUD overlay compositor |
| `0x004aaf60` `NovaUi_PresentStartupSplashFrame` | startup splash (PICT 0x83) |
| `0x004ab070` `NovaUi_PresentLoadingSplashFrame` | loading splash (PICT 0x1fa4) |
| `0x0048adc0` `IntroCinematic_Run` | new-game intro cinematic player via `DAT_00597950` |
| `0x004ac950` / `0x0046f740` | offscreen surface (`DAT_00597950`) allocation |
| `0x004ad960` | sprite table loader (menu focus 600–605, weapon 3000+, ship 400+) |
| `0x00470ee0` + `0x00471e10/0x00471e90` | sprite→surface blit (RLE) |
| `0x004b6850` (`NovaRender_CommitFrame`) / `QueuePresentAndSwap` | end-of-draw present |
| `0x00597950` / `0x00597954` | shared offscreen surface + rect |
| `0x00596cb8` | menu focus sprite array [6] |
