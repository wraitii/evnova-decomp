# Player ship swap / escort capture

Scope: `Player_ReplaceShipWithCapturedHull` (Ghidra 0x00423fa0, port
`src/game/landed_store.cpp`) and the boarding-window swap caller
(`NovaUi_RunBoardingPlunderWindow` 0x00482940, call at 0x00483c55, port
`src/game/boarding_plunder.cpp`). Loadout-transfer helper:
`Player_TransferCargoAndJunkToEscortByRatio` (0x00469810). Tint resolver:
`Ship_ResolveShipTintColor` (0x0046e470, port `src/game/ship_visual.cpp`).
Tracker row: `decomp-progress.tsv` 0x00423FA0.

## Behavior (0x00423fa0)

Promotes a captured hull to the player ship and demotes the old player hull
into a free escort slot. If `Ship_AllocateShipSlotInSystem` finds no slot the
swap is refused (STR# 0x7d2 0x131) and nothing changes; the promoted escort's
personality is marked dead. The only static caller (0x00483c55) passes
`flag == 0`; the other arm is the unused "scuttle the old hull" variant.

The swap is field-selective, not a struct copy:

- **Outgoing hull (new escort).** Inherits class, kinematics, cargo bins and
  afterburner/mining-scoop capability; clears all identity/mission state
  (`dude_class_id`, `pers_def_slot`, `mission_owner_slot`/`mission_fleet_slot`,
  `escort_command_code`), re-rolls voice (`rand(2)` over the class government's
  voice mode), and **truncates** the heading (x87 FIST, not round-to-nearest).
  `flag == 0` makes it a behavior-6 escort with full fuel and repairs armor
  *only when disabled* (`max_armor * 0.3333 + 1`, or `0.1` for capability flag
  0x10); `flag != 0` pins shield 0 / armor 1 and runs the loadout-transfer
  helper.
- **Follower repair.** Any active ship whose squad leader is the captured hull
  has target/hostility/leader cleared and reverts to its class default
  behavior; followers of the outgoing hull are untouched.
- **New player hull.** Inherits the captured hull's class, kinematics, cargo,
  random fuel and resolved paint, plus class-default outfits and weapon banks —
  but keeps the old player's credits, identity, mission state, `ai_state`
  control and cloak/jamming. Non-`persistent_on_ship_swap` slot-0 banks/outfits
  are cleared; the captured hull's non-empty NPC bank count is merged in
  (secondary remapped by `WeaponDef.ammo_type`; mode-99 keeps its bank).
  `OnRetire` (old class) then `OnCapture` (captured class) run after the
  outgoing hull is initialized; the captured hull is deactivated last and
  STR# 0x7d2 0x130 shown.

The player-visible surface is just the two overlay strings (0x130 retained /
0x131 refused) and the caller's rename prompt (0x77 confirm / 0x7a cancel).

## Caller arm (0x00482940)

After the capture-decision dialog returns "use as my ship":
- prompt = STR# 0x7d2 1-based 0x77 "Now rename this captured ship:";
- initial text = captured class display name (resource record name with the
  `;`-subtitle stripped, Ghidra `ShipClassDef+0x6c`) + " " + three digits
  `rand(9)+'1'` (1..9 each); max 0x40; via
  `NovaUi_ShowTextConfirmCodeDialog` (0x00497900, port
  `NovaUi_ShowTextEntryDialog` in `ui_dialog.cpp`);
- confirm: write the returned text to the player ship name, call
  `Player_ReplaceShipWithCapturedHull(target, 0)`, then
  `Ui_InstallGameplayInterfaceLayout` (0x004cda50, port `HudRenderer::Install`);
- cancel: overlay STR# 0x7d2 1-based 0x7a "You decided not to capture this ship
  after all.", no swap and no escort conversion.

## Tint (`Ship_ResolveShipTintColor` 0x0046e470)

Port `NovaShip_ResolveTintColor` reproduces the original exactly: default to the
current global paint, then for a non-player hull (`ship_instance_id != 0`)
override from the credited personality color, else the faction government's
`ShipColor`; all-zero falls back to neutral 0x20. Units are the original's raw
16-bit channels: PersDef/outfit paint is 5-bit, while the loader stores a
government's 8-bit `ShipColor` as `byte << 8`
(`NovaData_LoadScenarioResourceTables` 0x004bd3c0).

The swap writes the resolver result back to `state.ship_paint_rgb5`
(`DAT_00733b4a/4c/4e`). The outfit ModType-43 paint arm
(`NovaOutfit_GrantOutfitToPlayer` 0x00427770) now decodes the first ModVal's
15-bit RGB into the same 5-bit channels. The base hull draw consumes the paint
through `SpriteDrawOptions.tint_rgb5` (`DrawShipSprite`), rendered as an SDL
`SDL_SetTextureColorMod` multiply.

**Documented divergence:** the original's base-hull blitter
(`BlitPixel_TintRgb15Span` 0x004736c0 /
`SpriteRleCommandStream_BlitTintedRgb15` 0x00472900) computes
`out = src * (tint + 0x20 - base_transparency)/0x20 + overflow + dst * brightness/0x20`,
where any channel scale above 0x20 folds into an **additive** boost
(`overflow = ((scale - 0x20) * 31) >> 5` in 5-bit units) and
`brightness = class base_transparency` darkens the destination. This is a
pre-existing SDL renderer gap shared by the whole sprite pipeline, not specific
to the capture flow: the port reproduces only the multiplicative part
(0..0x20), does not model `base_transparency`, and clamps above-0x20 channels to
the neutral, so a government-colored hull renders untinted instead of
additively boosted. The resolver and global paint state are exact, so the
capture gameplay (paint selection, save persistence) is complete; the render
helper is not claimed faithful.

## Divergences

- The original's three global dirty flags (inventory/loadout, presentation,
  availability) map to `GameState::InvalidateDerivedStatCaches`.
- Additive tint overflow and `base_transparency` are not reproduced in SDL
  (formula above); NPC hulls use the same approximation.
- Reaction-script ordering is verified by citation, not a mutating test.
