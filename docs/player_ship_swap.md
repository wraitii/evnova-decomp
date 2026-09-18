# Player ship swap / escort capture

Scope: `Player_ReplaceShipWithCapturedHull` (Ghidra 0x00423fa0, port
`src/game/landed_store.cpp`) and the boarding-window swap caller
(`NovaUi_RunBoardingPlunderWindow` 0x00482940, call at 0x00483c55, port
`src/game/boarding_plunder.cpp`). Loadout-transfer helper:
`Player_TransferCargoAndJunkToEscortByRatio` (0x00469810). Tint resolver:
`Ship_ResolveShipTintColor` (0x0046e470, port `src/game/ship_visual.cpp`).
Tracker row: `decomp-progress.tsv` 0x00423FA0.

## Ground truth (0x00423fa0)

The only known static caller (0x00483c55) passes `flag == 0`; the `flag != 0`
arm is preserved regardless (indirect calls would not appear in this database).

1. `slot = Ship_AllocateShipSlotInSystem(current_system, 1)`; `-1` -> overlay
   STR# 0x7d2 0x131 ("You were unable to retain your old ship as an escort."),
   return.
2. If the promoted escort's `pers_def_slot != -1`, set
   `g_pers_defs[pers_def_slot].alive = 0`.
3. Initialize the replacement (outgoing player hull) from slot 0: class,
   `dude_class_id=-1`, `pers_def_slot=-1`, pos/vel/heading,
   `ai_desired_heading_deg = trunc(heading)` (x87 FIST, not round-to-nearest),
   targets -1, active bank -1, system, faction -1, `field_0xbb=0`,
   `mission_hail_latch=0`.
4. `afterburner_latch = Ship_CanShipUseAfterburner(replacement)`;
   `mining_scoop_active = Outfit_HasMiningScoopOutfit(replacement)`.
5. `mission_owner_slot=-1`, `escort_command_code=-1`,
   `ship_instance_id=slot`, voice = `rand(2)` then the class
   `inherent_attributes_govt` -> government `voice_type_mode` override when both
   `!= -1`.
6. Copy cargo bins 0..5 from slot 0 to the replacement.
7. **Leadership repair**: for active slots 1..0x3f other than the promoted
   escort and the replacement, if `other.squad_leader_ship_slot ==
   ship->ship_instance_id` (the *captured* hull) clear secondary/primary target,
   hostility, leader, and set `ai_behavior_code = class.default_ai_behavior`.
   Followers of slot 0 are untouched.
8. `replacement.mission_fleet_slot = -1`.
9. flag 0: `death_timer=0`, `squad=0`, `behavior=6`, `defense_home=-1`,
   `hostility=0`, fuel = `Ship_ComputeShipFuelCapacity(replacement)`, and
   **only if `Ship_IsShipDisabled(replacement)`** repair armor to
   `Ship_ComputeShipMaxArmor(replacement) * fraction + 1.0` (0.3333, or 0.1 for
   capability_flags 0x10); `squad_leader_slot_snapshot[slot]=0`,
   `Ship_ResetShipAiBehaviorRuntimeFields`, `Ship_EnterSquadReturnState`.
   flag!=0: shield 0, armor 1, boarded latch 1, `post_hit_mode_hint=-1`,
   `field_0xbb=0`, `mission_hail_latch=0`, `squad=0`, `behavior=6`,
   `Player_TransferCargoAndJunkToEscortByRatio(slot)`, then **replacement**
   `squad=-1`, `behavior=-1`.
10. Set the inventory/loadout, presentation and availability dirty flags.
11. Seed the replacement's NPC weapon banks from the old class defaults.
12. Run reaction scripts: `OnRetire` of slot 0's old class, then `OnCapture` of
    the captured class.
13. Rebuild slot 0 field-selectively from the captured hull: class, pos/vel/
    heading, truncated `ai_desired_heading_deg`, targets -1, active bank -1,
    `Ship_ResolveShipTintColor(captured)` (written back to the global paint),
    cargo bins, `rand(fuel_capacity)`, behavior -1, squad -1, faction -1,
    shield 0, armor pinned. Then: clear slot-0 banks for non-
    `persistent_on_ship_swap` outfits; merge the captured hull's nonzero
    `npc_weapon_count_by_class` where the slot-0 bank is empty (secondary
    remapped by `WeaponDef.ammo_type`, mode-99 keeps the bank); clear
    non-persistent `outfit_owned_count`; `ReconcileOutfitPoolWithWeaponBanks`;
    add class default outfits; `Weapon_RebuildWeaponBankPoolsFromOwnedOutfits`.
    Finally `captured->is_active = 0` and overlay STR# 0x7d2 0x130
    ("You retained your old ship as an escort.").

Slot 0 is NOT a whole-struct copy of the captured hull. Unlisted fields keep
the old player's values: `credits` (+0xA0), `pers_def_slot`, `dude_class_id`,
`mission_owner_slot`, `mission_fleet_slot`, `escort_command_code`,
`ai_state_code`/`ai_control_mode`, cloak/jamming, etc.

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

## Final implementation status

All structural gaps P1-P12 from the earlier inventory are reconstructed in
`Player_ReplaceShipWithCapturedHull` and the boarding caller: field-selective
slot-0 rebuild (credits/identity/mission state preserved), captured-hull
leadership target, flag!=0 outgoing-hull reset, script order after replacement
initialization, conditional outgoing-hull armor repair from
`Ship_ComputeShipMaxArmor`, both heading truncations, the missing replacement
fields (afterburner/mining/voice/`squad_leader_slot_snapshot`), the shared
`NovaShip_ApplyInherentGovernmentVoice` helper, the junk/unclamped-denominator
transfer helper, the persistent-bank captured-loadout merge, the promoted
personality `alive` clear, the rename/cancel dialog plus HUD reinstall, and the
0x130/0x131 overlays.

Tracker: 0x00423FA0 is a faithful reimplementation except for the tint
render-approximation boundary below.

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

Concrete design for the remaining render work: derive
`scale_c = clamp(resolved_c - base_transparency, 0, 0x20)` and
`boost_c = resolved_c - base_transparency > 0x20 ? ((resolved_c - base_transparency - 0x20) * 31) >> 5 : 0`
in `DrawShipSprite`; draw the hull with the existing multiplicative color mod
(`scale_c/0x20`), then draw the frame's white silhouette again with
`SDL_BLENDMODE_ADD` and color mod `boost_c/0x1f`, and for `base_transparency > 0`
first darken under the silhouette with a black `BLEND` pass at
alpha `1 - base_transparency/0x20`. That mirrors the original's
source-multiply + constant-add + destination-multiply without a shader.

## Remaining gaps / uncertainties

This function-local reconstruction is complete; the items below are
pre-existing, wider rendering-subsystem gaps or verification notes:

- The original's three global dirty flags (inventory/loadout, ship
  presentation, availability caches) are not modelled separately; the port uses
  `GameState::InvalidateDerivedStatCaches` for the derived stat caches.
- Script ordering is verified by code review/citation, not a field-mutating
  test: the control-expression engine only sets control bits.
- The additive tint overflow / `base_transparency` blend above is not
  reproduced in SDL (design recorded above).
- NPC hull tint uses the same multiplicative approximation (faction/personality
  colors in the >0x20 regime render neutral).
- `ref_audit.py` full-dump coverage is unavailable: `/tmp/ghidra_full_decompile`
  currently exists but contains no `.c` files, so only the row-disjointness and
  citation/name checks ran.

## Tests

`tests/landed_window_test.cpp`: credits/identity preservation, captured-only
followers, both heading truncations, both flag arms with the transfer
denominator quirks, captured weapon/ammo merge, source-bank secondary gate,
remapped-bank overwrite, mode-99 same-bank, outgoing-hull class-stock NPC banks,
afterburner/mining/voice fields, outgoing-hull armor flags, both reaction
scripts, and the four tint/paint resolver cases.
`tests/sprite_test.cpp`: an SDL software-render pixel test of the tint color
mod. `tests/boarding_strings_test.cpp`: STR# 0x7d2 0x77/0x7a/0x130/0x131.
