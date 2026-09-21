# Spaceport Bar & Escort Hiring

Ground truth for the Bar modal and the escort-hire flow, reconstructed from
Ghidra. The clean-room counterparts live in
`src/game/docked_bar_dialog.cpp` (`RunBarDialog`, `RunBarNewsWindow`), 
`src/game/landed_store.cpp` (hire lane), `src/game/ship_spawn.cpp`
(`NovaShipClass_SpawnEscortShipFromClass`), and `src/game/ship_ai.cpp`
(`NovaShip_EnterSquadReturnState`).

## The Bar modal (`NovaUi_RunTravelDestinationServicesWindow` 0x0047c8e0)

- Window `DAT_007d0690` built from **DLOG 0x3f5**, or **0x3fd** when the
  destination desc (**0-based stellar index + 10000**, i.e. the same desc
  family as Earth 10000 / Port Kane 10009; not the raw-id landing description)
  carries a
  `dialog_variant >= 0x80`; the variant is the art PICT, blitted into DITL
  entry 8. Backdrop PICT **0x2137** (plain) / **0x2138** (with art). Entry 7
  is the prompt panel; its text is the desc main text run through
  `Ui_LoadSelectionDialogResource` (0x004c6d50, placeholder expansion).
- Six-button action strip (`NovaUi_DrawTravelDestinationServicesButtons`
  0x004a2810 / hit-test 0x004a26e0, DITL entries 1..6). Labels come from the
  STR# 0x96 pool via `DAT_007d831a` = {0, 10, 0xb, *stale*, 0xc, *stale*}:
  **Leave, Gamble, Holovid, (inert), Hire Escort, (inert)**. Slots 3 and 5
  are never written by the function — and their DITL rects lie **below the
  DLOG bottom edge** (0x3f5: tops 214/227 vs a 185px window), so modal-window
  clipping hides them; only the four functional buttons are ever visible
  (the clean-room skips out-of-window rects for the same effect). Slot 4 (Hire Escort) is grey unless
  `Ship_CanPlayerHaveMoreEscorts` (0x00468920) allows it.
- Input callback `LAB_0047cdb0` (inside 0x0047c8e0): Esc/Enter → action 1
  (close); key arms g/r → action 2, w/n → action 3, h/e → action 5; key
  bindings 9/25/40 → actions 8 (starmap) / 9 (special) / 0xa (mission
  computer); clicks map through the button hit test. When the recheck timer
  `DAT_00776af4` expires it fires action 6 (the AvailLoc-1 offer pass,
  `Mission_RunAvailLocOffers`) and
  reseeds itself to `now + rand(0x1e) + 0x1e`.
- Action arms: 2 = Gamble — STR# 0x7d2 0x169 via the text-reader when
  `credits < 1`, else the gambling window (`NovaUi_RunBarGamblingWindow`
  0x0047dc50, renamed 2026 from the misleading "GoodsFlashWindow"; DLOG
  0x3ff, backdrop 0x2151, 4x4 flash-animation PICTs 0x2152..0x2181;
  wager = min(credits, 1000), or min(credits, 10000) rounded to a 2-digit
  quantum when the arm-modifier pair (0x38/0x6f — the Bet 1000/Bet 5000
  buttons) is held; deducts the wager, rolls rand(4) never repeating the
  previous roll, shows outcome desc 0x7ff8+roll, pays 4x on a match with the
  STR# 0x7d2 0x172 "Your winnings" text; instructions desc 0x7ffc; **not
  reconstructed yet**);
  3 = news window (below); 5 = Hire Escort: gate 0x00468920, then
  `g_shipyard_purchase_mode = 1` and `NovaUi_RunShipyardPurchaseLoop`
  (0x00492f30), i.e. the shipyard window (DLOG 0x3ec, PICT 0x2135) in hire
  mode.

## Holovid / news (`NovaUi_RunTravelNewsWindow` 0x0047d180)

- DLOG 0x3f6; news PICT = the destination government's `news_pic_id`
  (GovtDef +0x44), else PICT **9000** (fallback if the govt PICT fails to
  load). DITL 0x3f6 is the 2-byte `0xffff` placeholder in `Nova.rez`, so the
  window draws from the DLOG bounds plus the hardcoded panels below, not from
  DITL items.
- `NovaUi_ComposeTravelNewsTexts` (0x0047d600) composes two texts, called at
  bar entry: **headline** = random STR# 0x1fa4 (Commercials) entry, else
  STR# 0x7d2 0xbe ("No news is good news"); **body** = disaster-report text
  (defined öops record with >1 remaining day, preferring one at the current
  stellar or a `target_stellar_id` of -2; composed from STR# 0x7d2
  0xbe/0xc0/0xc1/0xc2/0xb5/0x3c + STR# 0xfa1 commodity name + host stellar
  name — ported as `Bar_ComposeDisasterReport`) or crön-event news
  (allied-govt STR# id at block +0x2a/+0x32, plain text id at +0x3a;
  **not reconstructed yet**), else random STR# 0x1fa5 (Generic News), else
  STR# 0x7d2 0xbf.
- Thumbnails: the shipyard list passes ShipClassDef +0xa0a
  `portrait_pict_resource_id` to `NovaUi_BlitPictThumbnailCached`
  (0x00497b70): PICT 5000+class when it exists, else the base-sprite owner's
  portrait (0x004aeda0) — never 5000+id blindly, or clone classes
  (Used Heavy Shuttle etc.) show the wrong hull; missing PICTs fill the
  atlas cell black.
- Draw (0x0047d370): PICT over the window rect; headline band
  (left+10, top+140, right-10, **top+180**); body panel
  (left+10, top+170, right-10, **bottom-4**) — both use
  `DrawContext_DrawPascalStringInFilledRect` (0x004bcd30) with the shared
  Geneva-9 screen font (DAT_00735684/86) and net a black panel with white
  wrapped text; the body is drawn second and covers the 10px overlap. The
  window has no DITL controls; close with Esc/Return or a click in the rect.

## Hiring

- **Capacity gate** `Ship_CanPlayerHaveMoreEscorts` (0x00468920): counts
  active ships with `ai_behavior_code == 6`, `squad_leader_ship_slot == 0`,
  `mission_fleet_slot == -1`; cap **6**. Shared with the capture keep-ship
  flow (port: `boarding_plunder.cpp`).
- **Hire listing** (`NovaUi_RebuildShipyardAvailabilityList` 0x00469e90, mode
  1): tech gate as usual, then the **+0xa2a threshold pair** —
  `HireRandom == 0` never offered; offered while the daily roll
  (`rand(100)+1`, rerolled by 0x00466cb0) `<= HireRandom`. The original
  multiplies the roll by the **shareware-license byte** (patterned as
  `ShipClassDef[g_expression_ship_class_id].is_licensed_runtime`, really a
  global set from the registration token at 0x00416100): unregistered builds
  multiply by 0 and offer every `HireRandom > 0` class. The port models the
  registered behavior only (logged divergence).
- **Purchase** (0x00492f30 hire arm of the confirm action): price =
  `Outfit_ComputeScaledPurchasePrice(base_cost, …) * DAT_00575950`, where
  **DAT_00575950 is the double 0.1** — escorts cost **10%** of the scaled
  ship price — deducted with the x87 rounding-fix dance (= round to
  nearest); credits may go negative. Then
  `ShipClass_SpawnEscortShipFromClass(class, destination_stellar)`, and the
  class's +0xa2a roll is rerolled to `rand(100)+1` (the hired class drops off
  the daily list). An empty hire list pops STR# 0x7d2 0xe0 ("There are no
  ships available for hire."); the main action button label is STR# 0x96 0xc
  ("Hire Escort") and the price panel shows "Hiring Price:" (0x7d2 0xe3).
- **Escort spawn** (`ShipClass_SpawnEscortShipFromClass` 0x00422400):
  `Ship_AllocateShipSlotInSystem(current_system, reserve 8)`, then
  `ai_behavior_code = 6`, `squad_leader_ship_slot = 0`, government −1,
  `random_ai_render_cadence = 2`, base shield/armor, afterburner latch
  (0x0046b260), mining-scoop latch (0x0046cb90), **+0xBB hired-origin mark
  set** (the comm dialog's "Captured Escort"/"Hired Escort" status gate),
  `mission_fleet_slot/mission_owner_slot = -1`, `escort_command_code = -1`,
  jamming caches −1, all eight weapon banks seeded from class stock.
  `spawn_stellar == -1` places it at the player with the player's heading,
  then scatters it by a random polar **position offset**
  (`Math_AddPolarVelocity` 0x0043b4a0 is called on the ship's `pos_x`/`pos_y`
  -- disasm 0x00422986 passes `ship + 0x18`, not the `+0x20` velocity pair --
  bearing `rand(0x168)°`, magnitude `50 + rand(0x32)` px); otherwise at the
  stellar's map position. Spawn velocity is left at the allocator's zero.
  Finally `Ship_ResetShipAiBehaviorRuntimeFields` (0x00402810) and
  `Ship_EnterSquadReturnState` (0x00410d10): escorts attached to
  the player enter **AI state 0x0c** (player-oriented assist/hold) with the
  secondary target mirroring the attach slot; behavior-5 followers of this
  ship re-enter state 0x05 (0x00410cb0 inline).

## Escort fleet behavior and management

- Hired escorts are `ai_behavior_code == 6` ships attached to the player
  (`squad_leader_ship_slot == 0`); the E-key escort-command overlay dispatches
  orders to them (`escort_commands.cpp`, order codes in `EscortOrder`).
- State 0x0c (`Ship_UpdateShipAiState` arm) is the hire default; the escort
  follow control mode is 0x08. Formation offsets (0x00413990) and payroll
  (0x004232d0) are wired, including jump-sync/arrival loss counting: jump
  payroll retains the maximum travel-day count and charges after arrival fleet
  restoration (0x0044fef8), with a dismissal text reader on shortfall. Normal
  launch restores player escorts with refill=true before mission and ambient
  population, matching 0x00457580's call to 0x0041af90.
- The management window (0x004853a0, DLOG 0x3fe / PICT 0x2141) shows the class
  portrait, captured/hired status, upgrade/sale values, and upkeep for hired
  ships. Release transfers cargo before detaching and resetting AI. Upgrade is
  enabled only when UpgradeTo resolves to a valid class whose Availability
  expression passes; sale is captured-only. The two scheduling marks are
  mutually exclusive and are consumed by the landed fleet pass. Options occupy
  DITL item 9, the portrait item 10, and identity details item 11. Text uses
  Geneva 9 at the original fixed baselines; monetary values use grouped digits
  and fixed value columns (+70 for upgrade/sale, +30 for pay).
- Command input, HUD, supervisor, and pilot persistence share the four-category
  order table. Closed-panel orders apply to all categories despite a retained
  selection; Attack sets state 4 even with an existing target. Non-recall orders
  clear a returning fighter's targets even when the order is unchanged. Group
  selection uses the original fixed number-row keys 1..5; slots 0x2b..0x2f are
  not category bindings. The 160x120 overlay sits at (15,150), uses fixed
  Geneva 9, stays opaque for 480 ticks after the last interaction, then fades
  over 32 ticks. It has `N) Group` rows and right-aligned order words. Either
  Alt key modifies Formation into Return to Hangar. The original +0xC4 cohort
  gate and acknowledgement chatter are not fully decoded.
- Cargo loss at unpaid-escort removal, destruction, disabled-escort adoption,
  and communications release uses the 0x00469810 helper. It **subtracts player
  commodities and junk** and writes the removed commodities to the recipient's
  six bins; it does not return cargo to the player. Payroll/destruction transfer
  before deactivation, retaining the recipient in the denominator. Adoption
  clears active first (0x0041b026), excluding that recipient. Destruction also
  reconciles the player's weapon/outfit pool. `tests/escort_fleet_test.cpp`
  covers the differing denominators and the combat-class/mission exclusions.
- Daily/launch re-rolls: escorts surviving into the next day re-roll
  availability only for the *listing*; hired ships persist (save/restore
  restores them through the same spawn path —
  `ShipClass_SpawnEscortShipFromClass` callers).
