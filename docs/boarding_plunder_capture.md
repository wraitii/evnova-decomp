# Boarding / Plunder / Capture (working doc)

Status: **in progress**. This doc is the map for reconstructing EV Nova's
boarding window system: the player "board target" command, the plunder modal
(DLOG 0x3f3), loot transfer, capture odds, escort conversion / ship swap, and
the surrounding hooks (escort cap, crew/marines, self-destruct panic, derelict
and unlicensed gates).

Ghidra functions covered (all currently 0% in `decomp-progress.tsv`):

| Address | Ghidra name | Role |
|---|---|---|
| 0x0045a3d0 | Ship_HandlePlayerBoardTargetCommand | player 'b' command: eligibility gates + dispatch |
| 0x00484230 | Ship_BuildBoardingPlunderOptions | pre-rolls the loot offers + capture odds |
| 0x00482940 | NovaUi_RunBoardingPlunderWindow | modal loop over DLOG 0x3f3; applies transfers |
| 0x00484d30 | NovaUi_DrawBoardingPlunderWindow | window painter |
| 0x004a22e0 | NovaUi_HandleBoardingPlunderOptionButtons | 6-button hit/hover tracking |
| 0x004a24e0 | NovaUi_DrawBoardingPlunderOptionButtons | three-state button strip |
| 0x00468920 | Ship_CanPlayerHaveMoreEscorts | behavior-6 escort count < 6 |
| 0x00497eb0 | NovaUi_ShowCaptureDecisionDialog | DLOG 0x3fa: escort vs swap |
| 0x00415cb0 | Ship_ResetShipAndAttackersAfterBoarding | clears targeting after capture |
| 0x004694a0 | ShipClass_HasPlayerBayCapacityFor | fighter-bay/escort-capacity check |

Port home: `src/game/boarding_plunder.hpp` (design exists) /
`src/game/boarding_plunder.cpp` (missing — the work).

---

## 1. Ground truth

### 1.1 Command gates (0x0045a3d0)

`Ship_HandlePlayerBoardTargetCommand` is the *board* command channel
(`g_playerBoardTargetCommandLatch`, one-shot per press). Order of checks:

1. Already latched → no-op.
2. No primary target → no-op.
3. Player cloak-visibility threshold active → silent no-op.
4. Target eligible: `field_0xb9 == 0` or (no mission fleet and
   `post_hit_mode_hint >= 0`), AND `Ship_IsShipFireRestricted(target)`,
   AND target active, same system, `pers_def_slot != 0x3ff`,
   AND not destroyed. On failure: error sound + STR# 0x7d2 **0x82**
   ("You can't board this ship."), overlay duration 0x168.
5. Velocity gate: `abs(dvel_x) > 0.5 || abs(dvel_y) > 0.5` (double 00575598 =
   0.5) → STR# **0x84** "You're moving too fast to board this ship."
6. Range gate: `abs(dpos_x) <= target_sprite_half_span_x * 0.5` and same for
   y (per-axis, using the target's sprite half-spans) → STR# **0x83**
   "You're not close enough to board this ship." (0.5 factor = 00575598.)
7. Heading gate: `abs(shortest_angle_delta(player, target)) <= 30°`, else
   try target+180° alignment (`<= 30°` again); fail → silent return.
8. Boardability: `target.mission_fleet_slot == -1` requires
   `class.crew (capture_power) >= 1` else STR# **0x82**. (Bible: "Ships with 0
   crew can't be boarded".)
9. Mission arms (TODO(decomp) in port): active-mission fleet bounty handling,
   cargo pickup (`Mission_TryConsumeMissionInteractionResources`), escort
   repair ("Escort repaired." 0x7e / "Fighter repaired." 0x7f).
10. Play "boarded" effect `_DAT_00591a74`, hide travel selection sprite,
    redraw viewport/radar, then dispatch by target class:
    - plain non-mission ship → **plunder window** (below).
    - mission ship with special flags → mission interaction window or
      plunder window (TODO(decomp)).
    - `post_hit_mode_hint == 0/-1` + capturable class → immediate escort/
      fighter conversion arms (post-hit surrender): behavior 5 + launch from
      carrier bay, or behavior 6 escort conversion with armor restore
      `max_armor * {0.3333|0.1} + 1.0` (00575588/78/80) and cargo transfer.
11. On return: `target.field_0xb9 = 1`, clear other ships targeting it.
12. Player velocity is matched to the target on success
    (`player.vel = target.vel`) before the dispatch.

### 1.2 Offer roll (0x00484230)

 booty_flags = DudeDef.booty_flags of target's dude (target.dude_class_id),
 else no cargo/credits offers from the dude path. Bits (Bible):
 0x1 Food, 0x2 Industrial, 0x4 Medical, 0x8 Luxury, 0x10 Metal,
 0x20 Equipment, 0x40 "carries money".

- **Credits** (DAT_007d17e0):
  - booty_flags & 0x40 → dude path: `credits = round(((cost/1000) * 0.025
    [+ rand(cost_scale) branch when > 2.0]) * 1000)`, min 1000.
    (00575910 = 0.025, 00575918 = 1000.0, 00575900 = 2.0 threshold.)
  - else mission-ship path (no 0x40): `booty_base_credits/1000 * 0.5`
    (005758a0), same >2.0 random-branch shape, no min-1000 rule.
  - `< 1` → -1 = no offer.
- **Cargo** (DAT_007d17d0/d2): only when `(booty_flags & 0xffbf) != 0`.
  Roll `rand(7)` until the bin matches a set booty bit (bin i ⇔ bit 1<<i,
  0x40 is the money bit and is NOT a cargo bin). Quantity: `rand((holds+1)/2)
  + (holds+1)/2` (half to full holds), from `ShipClassDef +0` field
  (`cargo_holds`). `< 1` → cargo_type = -1.
- **Ammo** (DAT_007d17d4/d6): count target weapon banks with stock > 0,
  weapon mode != 99, player can carry that weapon/ammo
  (`Weapon_HasMatchingWeaponAmmoCarried`). Pick one at random
  (`rand(0x100)` retry loop). No candidates → -1.
- **Fuel** (DAT_007d17d8): `rand(class.fuel_max/10) * 10` when fuel_max >= 1,
  else 0.
- **Capture odds** (g_capture_odds_percent):
  - effective crew = player class crew, + `0.1 * Σ crew` of every active
    behavior-6 escort targeting slot 0 with no mission fleet
    (00575920 = 0.1, truncating add), + Σ owned outfit "marines" quantities
    (outfit ModType 0x19 = 25 with ModVal > 0 contributes `ModVal * owned`).
    A parallel strength accumulator adds `0.1 * Σ strength` of the same
    escorts (default_ai_behavior > 2 gate applies to both).
  - odds = `round(crew / (target_class.crew * 10) * 100)` (00575928 = 10,
    005758f8 = 100).
  - + Σ `(-ModVal) * owned` for marine outfits with ModVal < 0 (negative =
    +odds, Bible "−1..−100 increase capture odds by this amount").
  - if `player_class.strength * 5 < strength_accumulator` (the escort-boosted
    strength sum, not crew) → +10.
  - + `5 - rand(0xb)` (±5 noise).
  - clamp [1, 75] (0x4b).
  - → 0 when: target govt flags_primary & 0x800 (derelict — "starts out
    disabled"), or `ShipClassDef[g_expression_ship_class_id].is_licensed_
    runtime == 0` (the class being evaluated; in this window = the target's
    class via the expression-eval scratch), or escort cap reached.

### 1.3 Window (0x00482940 / 0x00484d30 / DLOG 0x3f3)

- DLOG 0x3f3 = 309×198, backdrop PICT 0x2143, centred on playfield.
  DITL 0x3f3 items (0-based, decoded via NovaResource_LoadDialogItems):
  - [0] 91..217 × 166..191 — Abort (STR# 0x96 0x22)
  - [1] 110..199 × 110..135 — Cargo (0x27)
  - [2] 35..124 × 138..163 — Credits (0x28)
  - [3] 204..293 × 110..135 — Ammo (0x29)
  - [4] 11..298 × 7..103 — text panel (UserItem; filled black, rows drawn)
  - [5] 16..105 × 110..135 — Energy (0x2a)
  - [6] 129..275 × 138..163 — Capture Ship (0x2b)
  UiPanel entries are 1-based: buttons = entries 1..4 then 6..7 (skipping the
  text panel at entry 5); original hit code reads exactly that set.
- Text panel rows (x offsets from panel left, y offsets from panel top):
  - y+12: "Cargo:" (STR# 0x7d2 0x6d) | y+28: "Ammo:" (0x6e) | y+42: key hint
    (DAT_0072f1cc pstring through NovaCommand_TranslateByInputMap) |
    y+56: "Capture Odds:" (0x6f)
  - values at x+50: y+28 cargo qty "tons of" <commodity>, y+42 credits
    (grouped), y+56 ammo count + (plural|singular) weapon name (outfit
    attribute ModType 3 = weapon, ModVal = bank id), y+70: fuel label
    (DAT_0072d9cc pstring) at x+1, fuel qty at x+50, **STR# 0x7d2 0x70 drawn
    at x+120 as the "odds" row label** (decompile + disasm confirm
    `PUSH 0x70; PUSH 0x7d2; CALL Resource_DrawStringEntry` — that entry is
    "Oops! You tripped this ship's security self-destruct mechanism.", i.e.
    the original appears to draw a mismatched string here; we reproduce it
    faithfully and comment), odds% at x+195 + "%." + unlicensed note
    (`is_licensed_runtime == 0`: note pstring + STR# 30000 entry 1 —
    string not in our dump; see open questions).
  - No-offer values render dimmed (color DAT_00733b56) with STR# 0x7d2 0x14f.
- On open: roll `panic = rand(0x1a) + 0xf` (15..40), build offers, then the
  mission-ship free-outfit bonus arm (TODO(decomp)).
- Button actions (NovaUi_PollTravelScriptAction result codes):
  - 1 = Abort: reset panic to -1, beep, close.
  - 2 = Cargo: beep if no offer; else clamp quantity to free fleet cargo
    space (`Outfit_ComputeFleetCargoCapacity - cargo_and_junk_total`); if
    nothing fits → "You couldn't store any of the cargo..." (0x71); else
    "You stole all the <qty> <tons|ton> of <commodity>" (0x73 + 0x187 + 0x6c),
    add to player bin, clear offer. Panic ×2.0, re-arm.
  - 3 = Credits: "You stole all the <credits> credits" (0x74 + credit
    display + 0x6c), add to player credits. Panic ×1.25.
  - 4 = Ammo: find outfit with ModType 3, ModVal = bank; transfer up to the
    offer count while mass fits and ownership limits allow, incrementing the
    player's bank counters; "You stole all the <n> <weapon(s)>" (0x73) or
    "couldn't store any ammo" (0x74). Panic ×2.0.
  - 6 = Energy: fill player fuel from offer up to capacity difference;
    "You stole all the <n> fuel" style messages (DAT_0072d7cc/8cc pstrings).
    Panic ×1.5.
  - 7 = Capture: derelict govt → odds = -1; roll `rand(100) < odds` (fail →
    "Your attempt to capture this ship was unsuccessful." 0x7d, close);
    then 1/10 "Oops" self-destruct roll → shields/armor = 0, death timer
    armed (0x70 overlay, close); else escort-cap check ("maximum possible
    number of escorts." 0x7c); else if player class crew >= 1 →
    **NovaUi_ShowCaptureDecisionDialog** (DLOG 0x3fa 257×114, PICT 0x2144;
    item0 = swap, item1 = escort; labels TODO from DITL decode);
    - escort path: run target's OnCapture-ish reaction script
      (`Mission_ExecuteReactionScript` of ShipClassDef.field_0x3e9),
      set ai_behavior_code 6 / ai_target 0, armor = `max_armor * 0.5`
      (005758a0), clear faction/mission links, reset AI runtime fields,
      `Ship_ResetShipAndAttackersAfterBoarding`, "You assigned this ship to
      your fleet of escorts." (0x7a).
    - swap path: confirm-code dialog (rename, random 3 digits appended to
      class name; STR# 0x7d2 0x76/0x77), on confirm
      `Outfit_SwapPlayerShipWithEscort` + gameplay layout reinstall.
- After every action the window repaints and the loop continues until an
  action closes it. Loot actions set `local_223` (capture re-roll latch:
  next loop iteration rolls `rand(100) <= panic` → target self-destructs:
  shields/armor 0, death timer, STR# 0x7d2 0x70 overlay, window closes).

### 1.4 Support functions

- `Ship_CanPlayerHaveMoreEscorts` (0x00468920): count active ships with
  `ai_behavior_code == 6 && squad_leader_ship_slot == 0 &&
  mission_fleet_slot == -1`; cap 6.
- `Ship_ResetShipAndAttackersAfterBoarding` (0x00415cb0): for every ship
  whose primary target is the captured ship: reset ai_state/control, clear
  target slots, hostility, stellar target. Same reset on the ship itself +
  `pers_def_slot = -1`, `voice_type_mode = rand(2)` overridden by class
  inherent_attributes_govt voice mode.
- `ShipClass_HasPlayerBayCapacityFor` (0x004694a0): fighter-bay outfit /
  escort-capacity counting (used by the post-hit arms and swap gating).

## 2. Port design

- New TU `src/game/boarding_plunder.cpp` implementing:
  - `NovaBoarding_BuildOptions(state)` → `BoardingPlunderOptions` (header
    exists). Uses `state.rng` via the shared `NovaRandomRange` pattern
    (uniform_int_distribution, negotiation_dialog style), `ScenarioData`
    lookups, `ShipClass::crew` as capture_power, `Outfit` mod pairs for
    marines/weapon attrs, `PlayerInventory`.
  - `NovaBoarding_RunWindow(platform, audio, state)` → modal loop following
    the ship-comm dialog pattern: LoadPictTexture(0x2143) backdrop,
    ServicesButtonArt buttons at DITL rects, NovaFontCache text, hover/press
    tracking mirroring 0x004a22e0, transfers applied to GameState, HUD
    overlays via NovaHud_ShowOverlayMessage.
  - `NovaBoarding_HandleBoardTargetCommand(platform, state)` → gates +
    dispatch; plunder window only in this pass, mission arms TODO.
  - `NovaShip_CanPlayerHaveMoreEscorts(state)`.
  - minimal `Ship_ResetShipAndAttackersAfterBoarding` port (targeting
    clears; voice_type_mode only when that field exists in the port).
- Input hook: `FlightInput.board` edge ('b', the original default binding),
  wired in the spaceflight loop next to target_action.
- Strings via `NovaHud_LoadStringEntry(0x7d2, ...)` / `0x96` for button
  labels; commodity/weapon names from `ScenarioData` outfit/cargo tables.
- Divergences to log in code (TODO(decomp) markers):
  - mission-ship arms of the board command (interaction window, bounty,
    cargo pickup) — mission ship defs not modeled.
  - Capture-decision dialog (DLOG 0x3fa) + ship swap — planned second pass;
    escort-only in the meantime (header already notes this).
  - `g_expression_ship_class_id` license gate modeled as target class
    license runtime state (port: ScenarioData has no license runtime yet —
    treat all as licensed unless loaded; see open questions).
  - sounds: original queues centered-resource cues (g_transition_sound_
    handle_table); port plays through SdlAudio one-shots or logs skips.

## 3. Iterations

1. **DONE — Options + escort cap** — `NovaBoarding_BuildOptions`,
   `NovaShip_CanPlayerHaveMoreEscorts`, `Weapon_HasMatchingWeaponAmmoCarried`
   (0x00469230) quirk preserved. Divergence: `rand(0)` reseeds in the
   original and returns garbage; port returns 0 (fuel roll only).
2. **DONE — Board command** — `NovaBoarding_HandleBoardTargetCommand`
   (gates + velocity match + plain-ship dispatch + denial overlays + beeps),
   `NovaBoarding_ResetShipAndAttackersAfterBoarding`, `FlightInput.board`
   ('b'), spaceflight hook, `pending_ui_sounds` queue + transition-sound
   cache (snd 150..155 = g_transition_sound_handle_table[0..5]).
3. **DONE — Window** — modal loop, painter, loot transfers, panic odds.
   `NovaBoarding_RunWindow` (0x00482940) now renders DLOG 0x3f3 (backdrop PICT
   0x2143, six three-state buttons from DITL 0x3f3, the DITL item-4 text panel
   with the offer rows incl. the "self-destruct string as odds-row label" quirk),
   and applies the cargo/credits/ammo/fuel transfers plus the panic self-
   destruct re-roll ladder. Draw helpers port 0x00484d30 / 0x004a24e0; button
   hit/hover filtering mirrors 0x004a22e0. Commodity names load from STR# 0xfa1
   (entry cargo_type+1, the original's DAT_0069d2cc source); weapon names from
   Outfit LCName/LCPlural.
4. **Modal background + trip diagnostics** — the plunder window renders the
   LIVE game view beneath itself every frame: `SpaceflightView::DrawGameFrame`
   (the former `DrawInGameFrame` — fullscreen world draw + HUD + fire flash)
   runs, then the window composites over it in the centred 640x480
   presentation. No pixel snapshots and no scrim: the original draws its DLOG
   over the unmodified gameplay surface, and the HUD's overlay-message rect
   (loot / "Oops!" text) stays visible at its normal bottom-of-window spot
   while the window is open. The flight simulation itself is paused during
   the modal (the dispatch runs the window synchronously again; the
   dispatch/post-present split was reverted). Pixel-read snapshots
   (`SDL_RenderReadPixels` after present) were tried and abandoned: the
   backbuffer is undefined after present and the reads came back mostly
   black/mis-scaled. Also added action/roll/close logging (`board: action …`,
   `panic re-roll survived/tripped`, `window closed (…)`). Note verified
   against the decompile: the panic self-destruct re-roll is ONE
   `rand(100) <= panic` check on the loop iteration after each *successful*
   loot action (the original clears its latch every iteration). Trips on loot
   clicks (15–40% base, panic ×2/×1.25/×1.5 per action) and capture-fail
   closes at low odds are authentic original behavior.
4b. **Overlay lifetime + range gate corrections** — `NovaHud_ShowOverlay-
   Message`'s second parameter is a FRAME countdown, not ms/colour:
   `g_hud_overlay_msg_color` doubles as the counter and
   Frame_UpdateScreenFlashTimers (0x0042f1b0) decrements it per frame,
   clearing the message at zero. That decay runs on the 30 Hz
   Frame_TickSystems cadence (the port renders at ~60 Hz but ticks the sim on
   the same 30 Hz basis), so the port converts ticks to wall-clock ms at
   1000/30: board denials 0x168 ≈ 12 s, window loot/trip overlays 0xf0 ≈ 8 s;
   previously these rendered as 250–360 ms, far too short). The window now
   also draws the live overlay message below itself (`HudRenderer::
   DrawOverlayMessage`) as the original's message rect does. Boarding range
   gate fixed: the sh\x8an descriptor is loaded at `ship_class_id + 0x80`
   (the renderer's id convention; the old code silently fell back to the
   collision radius), and `Sprite_GetFrameFullHeight` /
   `Sprite_GetFrameFullWidth` return the FULL frame spans (bounds
   subtraction, default 0x20), so the gate is half the full frame per axis,
   not half of the half-frame (was 2× too strict).
5. **Capture arm** — escort conversion + reset-after-boarding. DONE: the
   window takes the escort-conversion path (behavior 6, armor restore,
   reset-after-boarding).
6. **Capture-decision dialog + swap** (0x00497eb0) — dialog DONE:
   `RunCaptureDecisionDialog` renders DLOG 0x3fa (257x114, backdrop PICT
   0x2144, item-2 panel STR# 0x7d2 0x75 word-wrapped, binary-choice buttons
   "Use As My Ship" / "Use As Escort" from STR# 0x96 0x2e/0x2d via the shared
   three-state art) over the still-open boarding window; both choices play
   transition-table [1]; shown only when the player class capture_power
   (crew) >= 1. Strings pinned by tests/boarding_strings_test.cpp. The
   "Use As My Ship" arm is TODO(decomp(0x00497eb0)) skipped — the rename-
   confirm + Outfit_SwapPlayerShipWithEscort machinery (docked-loadout
   transfer + interface reinstall) is deferred; the escort conversion is the
   port's fallback for both choices (logged).
7. **progress.csv + doc updates** — updated per iteration (1/2 landed).
8. **AI boarding resolution (0x00412550) + capture-variant supervisor
   (0x004038b0)** — DONE, wired:
   - `NovaBoarding_BoardShipAndTransferCargo` (boarding_plunder.cpp) ports
     the AI boarding resolution: capture-odds model (NOTE: the AI version
     reads ShipClassDef.**strength** (+0xc) as the base crew on both sides —
     NOT capture_power/crew like the player window — plus marine outfits
     ModType 0x19; odds = crew*100/(crew*2), ±10 noise, clamp [10,100];
     boarder-side negative marines raise the odds, victim-side lower them,
     all via 16-bit wrap arithmetic congruent to plain signed adds), the
     credits share vs the player (odds*29*1e-4 — disasm 0x00412d6a gives 29,
     not the decompiler's 0x1e=30; doubles 00575020 = 0.01), the loot HUD
     overlay ("<n> tons of cargo [and <c> credits] stolen!", 400 frames) +
     transition-table[1] voice, the conversion roll (odds<41 → cheat-only;
     else rand(0x65) <= odds*0.5), faction conversion to a behavior-6
     follower of the boarder (armor = max*0.66, shields 0, latch/AI reset,
     "Escort/Fighter stolen!" overlay + victim targeter reset), and the
     mission-failure arm (flags_primary 0x8000) when the player is boarded.
     Ghidra updates: full plate comment on 0x00412550, and the shared
     constants 00575020/005751a0 retyped double + renamed
     CONST_0_01_f64 / CONST_2_0_f64.
     Divergence: the bin-to-bin cargo plunder's NPC-victim/boarder bins
     remain unmodelled (port models bins only on PlayerInventory), so the
     loot overlay reports only the credits share plus any cargo plundered
     from a player victim; see iteration 9.
   - `NovaAi_UpdateBehavior0x03CaptureVariant` (ship_ai.cpp) ports the
     supervisor end to end (0x16 guard, disabled-ship scan via
     NovaAi_SelectNearestDisabledShipForBoarding, acquire/travel ladder,
     capture-approach 0xd vs attack 4 arbitration on crew/capturable kind,
     boarded-latch conflict scan -> yield 0x16 with timer 120, the mode-0xf
     boarding handoff, the no-fireable-weapon abandon arm, the ammo-depleted
     stand-down) and the dispatcher now selects it when the faction's
     government flags_primary has 0x1000 (Bible: "warships will plunder
     non-mission, trader-type enemies"). Plate comment refreshed on
     0x004038b0.
   - Remaining gap: the approach drive (state 0xd -> control 0xf -> timer
     expiry) lives in the partially ported state machine
     (NovaAi_UpdateShipState / ApplyControls); until that slice lands the
     handoff may not trigger end-to-end in-game. Needs probe verification.

9. **Cargo plunder + profile jettison** — DONE:
   - `NovaBoarding_BoardShipAndTransferCargo` (0x00412550) now ports the
     random-bin cargo plunder for a player victim: the boarder's free holds
     are its class `cargo_holds` (its own bins are unmodelled), the victim's
     capacity is `Outfit_ComputePlayerTotalMass` (the original's
     `Ship_ComputeShipTotalMass`), and bins transfer one at a time into the
     free-holds budget. NPC victim bins stay zero (matching
     `Ship_AllocateShipSlotInSystem`). The loot overlay now reports the
     stolen tonnage. `tests/cargo_test.cpp`.
   - `NovaOutfit_RedistributeFleetCargoOverflow` (0x0041f330) is ported in
     `src/game/outfit.cpp`: clears the six player cargo bins and every
     positive junk count; `jettison_all` also drains abortable active
     missions' cargo and fails them with STR# 0x7d2 0x11c "Mission failed.",
     then shows entry 0x121/0x122 ("Cargo jettisoned." /
     "Non-mission cargo jettisoned.") and queues transition-sound 4. The
     Player Info Cargo-page Jettison confirmation applies it through the
     flight loop (0x00499c10's call); the in-flight arm-modifier + slot 0x0f
     channel (0x0044aa70 block 0x00451907, Shift = non-mission only) is wired
     too. It then spawns the visible jettisoned-cargo pods through the new
     `FreeflightObjectState` pool (see iteration 10).
   - HUD `DrawCargoPanel` (0x004612c0) now names the single held junk type
     from `JunkDef::abbrev` (the g_junk_defs +0x128 field) instead of
     leaving the "Special:" value blank.

10. **Visible jettisoned cargo pods** — DONE:
   - New `src/game/freeflight_objects.{hpp,cpp}` ports the 64-slot
     `FreeflightObjectState` pool: `Ship_SpawnFreeflightObjectForShip`
     (0x0041f800), `Ship_SpawnFreeflightObjectAtPosition` (0x0041fb50, no
     callers yet) and the simulation half of `Frame_UpdateFreeflightObject-
     Sprites` (0x0042c1b0) as `NovaFreeflight_Tick`. The jettison pass spawns
     ROUND(share/5) clamped [1,12] pods per eligible hull through the
     player's and escorts' backward launch scatter.
   - `SpaceflightView::DrawFreeflightObjects` draws each live object's
     `500+index` spin set (index 0 = the stock cargo/junk set; pinned as a
     36-frame sheet by `tests/cargo_test.cpp`), frame from the tick's
     accumulator and alpha faded over the final 32 ticks, filtered to the
     current system.
   - TODO(decomp): the original's `DAT_00596d2a` clear-transient-sprites
     latch and `Frame_UpdateSpriteDistanceIntensity` distance dimming are
     not modelled.

### In-game verification (iterations 1-2)
To exercise this today: fly, target a disabled hostile ('`' to cycle), slow
to < 0.5 px/frame relative velocity, align heading within 30°, close to half
the target's sprite frame, press **b**.

- Confirmed working in-game: gates pass on a disabled target and the stub
  logs the rolled offers (observed: `cargo(-1) credits=-1 fuel=30
  capture_odds=10%` — a booty-flagless dude, so no cargo/money offers, while
  fuel and odds still roll from class stats, matching the original's
  independence of the two paths).
- Denial checks: board an undamaged ship → "You can't board this ship.";
  board at speed → "You're moving too fast..."; board far away → "You're not
  close enough..."; wrong heading → silent no-op (original behavior).
- Each denial also beeps (snd 153).
- Reset-after-boarding is not reachable until iteration 4 (capture arm).

## 4. Open questions / to verify in-game

- Player death is not handled by the flight sim yet: a destroyed player can
  keep flying (armor <= 0, no game-over/destruction sequence). This surfaced
  during boarding tests — the board command correctly denies while destroyed
  (Ship_IsShipDestroyed gate) but the sim itself never ends the run.
- Boarding dispatch divergence: the original gates the plain-ship dispatch
  through ShipClass_HasPlayerBayCapacityFor (0x004694a0) +
  post_hit_mode_hint, routing fighter-class targets to the "Fighter
  captured." (pool 0x80) / "Fighter repaired." (pool 0x7f) carrier arms
  instead of the plunder window; the port always opens the window.

- STR# 30000 entry 1 (the "unlicensed ship" note appended after the capture
  odds when the class is unlicensed) — pool not found in the archives dump;
  re-check whether any archive carries STR# 0x7530 and what its text is.
- Mission-ship boarding behaviors for later iterations.
- Capture-decision DITL 0x3fa button labels (decode pending).
- RESOLVED (corrected 2nd time): the bottom odds-row label is STR# 0x7d2 entry
  "Capture Odds:" (pool 0x6f), NOT the "Oops!" self-destruct string. An earlier
  note here claimed the engine cheekily drew the Oops string as the odds-row
  label "confirmed in-game" — that was a misreading. Root cause: the game's
  STR# entry helpers (Resource_LoadStringEntry 0x004b8ca0 / DrawStringEntry
  0x004cd1f0 / AppendStringEntry 0x004cd1a0) take a 1-BASED index
  (Resource_LoadStringEntry walks `param_3 - 1` length-prefixed strings to skip
  — see its decompile), and the port's NovaHud_LoadStringEntry is 1-based too
  (normalized 2024; every call site passes the original's entry value
  verbatim). I.e. code value = pool index + 1: window rows pass
  0x6d..0x70 = pool title/"Cargo:"/"Ammo:"/"Capture Odds:", the self-destruct
  overlay passes 0x71 = pool 0x70 "Oops! ...", the cargo-full overlay passes
  0x72 = pool 0x71, "of" is 0x187 = pool 0x186, the fuel overlays pass 4/5/6 =
  pool 3/4/5, and the board denials use pool 0x81/0x82/0x83 "You can't board
  this ship." / "You're not close enough to board this ship." / "You're moving
  too fast to board this ship." (entries 0x82/0x83/0x84; 0x84 confirmed at
  Ship_HandlePlayerBoardTargetCommand 0x0045a3d0). Beware: some older Ghidra
  plate comments list pool indices while claiming they are the 1-based call
  values — trust the decompile call sites. The shipped window layout is
  docs/reference/boarding.jpg.
