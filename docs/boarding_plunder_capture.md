# Boarding / Plunder / Capture

Reference for EV Nova's boarding window system: the player "board target"
command, the plunder modal (DLOG 0x3f3), loot transfer, capture odds, escort
conversion / ship swap, and the surrounding gates (escort cap, crew/marines,
self-destruct panic, derelict and unlicensed gates).

Ghidra functions covered:

| Address | Ghidra name | Role |
|---|---|---|
| 0x0045a3d0 | Player_HandleBoardTargetCommand | player 'b' command: eligibility gates + dispatch |
| 0x00484230 | Boarding_BuildOptions | pre-rolls the loot offers + capture odds |
| 0x00482940 | NovaUi_RunBoardingPlunderWindow | modal loop over DLOG 0x3f3; applies transfers |
| 0x00484d30 | NovaUi_DrawBoardingPlunderWindow | window painter |
| 0x004a22e0 | NovaUi_HandleBoardingPlunderOptionButtons | 6-button hit/hover tracking |
| 0x004a24e0 | NovaUi_DrawBoardingPlunderOptionButtons | three-state button strip |
| 0x00468920 | Ship_CanPlayerHaveMoreEscorts | behavior-6 escort count < 6 |
| 0x00497eb0 | NovaUi_ShowCaptureDecisionDialog | DLOG 0x3fa: escort vs swap |
| 0x00415cb0 | Boarding_ResetShipAndAttackersAfterBoarding | clears targeting after capture |
| 0x004694a0 | ShipClass_HasPlayerBayCapacityFor | fighter-bay/escort-capacity check |
| 0x00412550 | Boarding_BoardShipAndTransferCargo | AI boarding resolution |
| 0x004038b0 | NovaAi_UpdateBehavior0x03CaptureVariant | capture/plunder supervisor |

Port home: `src/game/boarding_plunder.{hpp,cpp}`; ship swap in
`docs/player_ship_swap.md`. Coverage: `decomp-progress.tsv`.

## 1. Ground truth

### 1.1 Command gates (0x0045a3d0)

`Player_HandleBoardTargetCommand` is the *board* command channel
(`g_playerBoardTargetCommandLatch`, one-shot per press). Order of checks:

1. Already latched → no-op.
2. No primary target → no-op.
3. Player cloak-visibility threshold active → silent no-op.
4. Target eligible: `field_0xb9 == 0` or (no mission fleet and
   `fleet_recovery_hint >= 0`), AND `Ship_IsShipFireRestricted(target)`,
   AND target active, same system, `pers_def_slot != 0x3ff`,
   AND not destroyed. On failure: error sound + STR# 0x7d2 **0x82**
   ("You can't board this ship."), overlay duration 0x168.
5. Velocity gate: `abs(dvel_x) > 0.5 || abs(dvel_y) > 0.5` (double 00575598 =
   0.5) → STR# **0x84** "You're moving too fast to board this ship."
6. Range gate: `abs(dpos_x) <= target_sprite_half_span_x * 0.5` and same for
   y (per-axis, using the target's sprite half-spans) → STR# **0x83**
   "You're not close enough to board this ship."
7. Heading gate: `abs(shortest_angle_delta(player, target)) <= 30°`, else
   try target+180° alignment (`<= 30°` again); fail → silent return.
8. Boardability: `target.mission_fleet_slot == -1` requires
   `class.crew (capture_power) >= 1` else STR# **0x82**. (Bible: "Ships with 0
   crew can't be boarded".)
9. Mission arms: active-mission fleet bounty handling, cargo pickup
   (`Mission_TryConsumeMissionInteractionResources`), and the rescue/board
   completion line (STR# 0x7d2 0x7e, or the flags-0x0008 class-name voice line
   below).
   - **Rescue / board-captain** (ShipGoal 2/5, Flags 0x0001, a single special
     ship): the completion line is STR# 0x7d2 **0x7e** ("Target ship has been
     boarded."). When mission **Flags 0x0008** is also set (the Bible's
     100-fuel auto-abort penalty, i.e. a fuel-transfer mission), the overlay
     becomes the class-name voice line `"<class>:  <STR# 0x7d2 entry 3>,
     <player ship>."` (Ghidra 0x0045ae40). Entry 3 is preloaded into
     `DAT_0072d5cc` by `NovaData_LoadDisplayNamePstringTables` (0x004c7040)
     as "Energy transfer complete" — the same line the AI refuel overlay at
     0x00407b92 uses. Both arms show for 0xfa frames.
10. Play the "boarded" effect `_DAT_00591a74`, hide the travel selection sprite,
    redraw viewport/radar, match the player velocity to the target, then
    dispatch. The main recovery/plunder arm runs for a plain hull
    (`mission_fleet_slot == -1`) or an active-failed mission hull, except the
    Shareware Enforcer personalities 0x3ff/0x3fe. A separate plain-hull
    fallback still plunders 0x3fe and propagates hostility; 0x3ff fails the
    earlier boardability gate. For a live (active, not failed) mission ship,
    the original sets boardability true and falls through to the latch with no
    window (it does *not* error). The main arm branches on the target personality:
    - `pers_def_slot == -1` → generic fleet-recovery arms by
      `fleet_recovery_hint`: `0` + bay room → 0x80 fighter recovery; `-1` + bay
      room → 0x81 capture; `>= 1` + escort room → behavior 6 escort rejoin with
      armor restore `max_armor * {0.3333|0.1} + 1.0` (00575588/78/80) and cargo
      transfer; otherwise plunder with `PropagateHostilityFromAttack`.
    - `link_mission_id == -1` → hint-0 + bay room → 0x80 recovery, else plunder
      with `PropagateHostilityFromAttack`.
    - `link_mission_id != -1`, PersDef Flags 0x0200 clear → hint-0 + bay room →
      0x80 recovery, else plunder with no hostility.
    - `link_mission_id != -1`, Flags 0x0200 set → if
      `Mission_CheckMissionShipInteractionEligibility` (0x00441b40) passes, run
      `NovaUi_RunMissionOfferWindow` (0x00442510) and retire the personality
      when Flags 0x0100 is set; an ineligible def falls through to the plunder
      window, and a declined/failed offer returns before the boarded latch.
    The Flags 0x0200 gate in the board command is the **opposite polarity** from
    the hail command (0x00454910), which runs the offer window when 0x0200 is
    clear.
11. On return: `target.field_0xb9 = 1`, clear other ships targeting it. A
    declined/failed mission offer and a failed cargo consume return before this
    latch (the decompile's `bVar4`).

`fleet_recovery_hint` (ShipState +0xC8DE) is written when a player-squad hull
is disabled/surrenders, recording its fleet origin so recovery can route:
`-1` = no fleet origin (cleared every live frame; a plain disabled hull),
`0` = player bay fighter, `1`/`2` = player escort (by `escort_origin_mark`).

### 1.2 Offer roll (0x00484230)

`booty_flags = DudeDef.booty_flags` of the target's dude (target.dude_class_id),
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

The AI boarding variant (`Boarding_BoardShipAndTransferCargo`, 0x00412550) uses
a **different** odds model: it reads `ShipClassDef.strength` (+0xc) as the base
crew on both sides (not `capture_power`/crew), plus marine outfits ModType 0x19;
`odds = crew*100/(crew*2)`, ±10 noise, clamp [10,100]. Boarder-side negative
marines raise the odds, victim-side lowers them (16-bit wrap arithmetic
congruent to plain signed adds). The credits share vs the player is
`odds*29*1e-4` (disasm 0x00412d6a gives 29, not the decompiler's 0x1e=30;
doubles 00575020 = 0.01). The conversion roll is `odds<41 → cheat-only; else
rand(0x65) <= odds*0.5`, converting the victim to a behavior-6 follower of the
boarder (armor = max*0.66, shields 0, latch/AI reset). `flags_primary 0x8000`
triggers mission failure when the player is boarded.

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
  text panel at entry 5); the original hit code reads exactly that set.
- Text panel rows (x offsets from panel left, y offsets from panel top):
  - y+12: "Cargo:" (STR# 0x7d2 0x6d) | y+28: "Ammo:" (0x6e) | y+42: key hint
    (DAT_0072f1cc pstring; first byte through the C-locale toupper
    MWRuntime_ToUpper, so it draws "Credits:") |
    y+56: "Capture Odds:" (0x6f)
  - values at x+50: y+28 cargo qty "tons of" <commodity>, y+42 credits
    (grouped), y+56 ammo count + (plural|singular) weapon name (outfit
    attribute ModType 3 = weapon, ModVal = bank id), y+70: fuel label
    (DAT_0072d9cc pstring) at x+1, fuel qty at x+50, odds% at x+195 + "%." +
    unlicensed note (`is_licensed_runtime == 0`: note pstring + STR# 30000
    entry 1).
  - No-offer values render dimmed (color DAT_00733b56) with STR# 0x7d2 0x14f.
- On open: roll `panic = rand(0x1a) + 0xf` (15..40), build offers, then the
  mission-ship free-outfit bonus arm.
- Button actions (NovaUi_PollTravelScriptAction result codes):
  - 1 = Abort: reset panic to -1, beep, close.
  - 2 = Cargo: beep if no offer; else clamp quantity to free fleet cargo
    space (`Player_ComputeFleetCargoCapacity - cargo_and_junk_total`); if
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
    item0 = swap, item1 = escort; item-2 panel STR# 0x7d2 0x75; labels
    "Use As My Ship" / "Use As Escort" from STR# 0x96 0x2e/0x2d);
    - escort path: run the target's OnCapture reaction script
      (`Mission_ExecuteReactionScript` of ShipClassDef.field_0x3e9),
      set ai_behavior_code 6 / ai_target 0, armor = `max_armor * 0.5`
      (005758a0), clear faction/mission links, reset AI runtime fields,
      `Boarding_ResetShipAndAttackersAfterBoarding`, "You assigned this ship to
      your fleet of escorts." (0x7a).
    - swap path: confirm-code dialog (rename, random 3 digits appended to
      class name; STR# 0x7d2 0x76/0x77), on confirm
      `Player_ReplaceShipWithCapturedHull` + gameplay layout reinstall.
- After every action the window repaints and the loop continues until an
  action closes it. Loot actions set `local_223` (capture re-roll latch: next
  loop iteration rolls `rand(100) <= panic` → target self-destructs: shields/
  armor 0, death timer, STR# 0x7d2 0x70 overlay, window closes).

**String indexing.** The game's STR# entry helpers (`Resource_LoadStringEntry`
0x004b8ca0 / `DrawStringEntry` 0x004cd1f0 / `AppendStringEntry` 0x004cd1a0)
take a **1-based** index (code value = pool index + 1). So the window rows pass
0x6d..0x70 = pool titles "Cargo:"/"Ammo:"/"Capture Odds:"; the self-destruct
overlay passes 0x71 = pool 0x70 "Oops! You tripped this ship's security
self-destruct mechanism."; the board denials pass 0x82/0x83/0x84 = pool
0x81/0x82/0x83. Some older Ghidra plate comments list pool indices while
claiming they are the 1-based call values — trust the decompile call sites. The
shipped window layout is `docs/reference/boarding.jpg`.

### 1.4 Support functions

- `Ship_CanPlayerHaveMoreEscorts` (0x00468920): count active ships with
  `ai_behavior_code == 6 && squad_leader_ship_slot == 0 &&
  mission_fleet_slot == -1`; cap 6.
- `Boarding_ResetShipAndAttackersAfterBoarding` (0x00415cb0): for every ship
  whose primary target is the captured ship: reset ai_state/control, clear
  target slots, hostility, stellar target. Same reset on the ship itself +
  `pers_def_slot = -1`, `voice_type_mode = rand(2)` overridden by class
  inherent_attributes_govt voice mode.
- `ShipClass_HasPlayerBayCapacityFor` (0x004694a0): fighter-bay outfit /
  escort-capacity counting (used by the fleet-recovery arms and swap gating).

## 2. Port contract and divergences

- `Boarding_BuildOptions(state)` → `BoardingPlunderOptions`; `state.rng` via the
  shared `NovaRandomRange` pattern, `ScenarioData` lookups, `ShipClass::crew` as
  capture_power, `Outfit` mod pairs for marines/weapon attrs, `PlayerInventory`.
- `NovaUi_RunBoardingPlunderWindow(platform, audio, state)`: modal over the LIVE
  game view (`SpaceflightView::DrawGameFrame` beneath, no snapshot/scrim), DLOG
  0x3f3 backdrop PICT 0x2143, three-state buttons at the DITL rects, hover/press
  tracking mirroring 0x004a22e0, HUD overlays via `NovaHud_ShowOverlayMessage`.
  Overlay lifetimes are FRAME counts on the original 21 ms cadence (board
  denials 0x168 ≈ 7.56 s, loot/trip overlays 0xf0 ≈ 5.04 s).
- `Player_HandleBoardTargetCommand(platform, state)`: gates + dispatch.
- `NovaShip_CanPlayerHaveMoreEscorts(state)`, a minimal
  `Boarding_ResetShipAndAttackersAfterBoarding` port, and the capture-decision
  dialog + swap (see `docs/player_ship_swap.md`).
- Range gate uses the `sh\x8an` descriptor loaded at `ship_class_id + 0x80`;
  `Sprite_GetFrameFullWidth`/`FullHeight` return the FULL frame spans (bounds
  subtraction, default 0x20), so the gate is half the full frame per axis.
- `NovaAi_UpdateBehavior0x03CaptureVariant` (ship_ai.cpp) ports 0x004038b0 end
  to end (0x16 guard, disabled-ship scan, acquire/travel ladder,
  capture-approach 0xd vs attack 4 arbitration, boarded-latch conflict yield
  with timer 120, mode-0xf handoff, no-fireable-weapon abandon, ammo-depleted
  stand-down); the dispatcher selects it when government `flags_primary` has
  0x1000 ("warships plunder non-mission trader-type enemies").
- Cargo plunder for a player victim (0x00412550): the boarder's free holds are
  its class `cargo_holds` (its own bins are unmodelled), the victim's capacity is
  `Ship_ComputeShipTotalCargoCapacity`, bins transfer one at a time into the
  free-holds budget. NPC victim bins stay zero, matching
  `Ship_AllocateShipSlotInSystem`.
- `Player_RedistributeFleetCargoOverflow` (0x0041f330) in outfit.cpp clears the
  six player cargo bins and every positive junk count; `jettison_all` also
  drains abortable active-mission cargo and fails them with STR# 0x7d2 0x11c,
  then shows 0x121/0x122 and queues transition-sound 4. It spawns visible
  jettisoned-cargo pods via the `FreeflightObjectState` pool
  (`src/game/freeflight_objects.{hpp,cpp}`: `Ship_SpawnFreeflightObjectForShip`
  0x0041f800, `Ship_SpawnFreeflightObjectAtPosition` 0x0041fb50, and the
  simulation half of `Frame_UpdateFreeflightObjectSprites` 0x0042c1b0 as
  `NovaFreeflight_Tick`; ROUND(share/5) clamped [1,12] pods).
- Sounds queue through the shared `pending_ui_sounds` channel / transition-sound
  cache (snd 150..155 = g_transition_sound_handle_table[0..5]).

### Documented divergences

- The carrier/escort recovery and personality dispatch matches the original
  branch structure (generic personality-less arms; personality
  `link_mission_id == -1`; Flags 0x0200 clear; Flags 0x0200 set offer window).
  The board command's Flags 0x0200 arm runs the offer window, the reverse of the
  hail command's polarity; both are recorded in their Ghidra plate comments. A
  live (active, not failed) mission ship falls straight to the boarded latch.
  The failed pickup-mode-2 cargo consume shows the generic STR# 0x7d2 0x82
  denial after the mission's 0x165/0x166 dialog, matching 0x0045ad60.
- `g_expression_ship_class_id` license gate is modelled as target-class license
  runtime state (ScenarioData has no license runtime; treat all as licensed
  unless loaded).
- The bin-to-bin cargo plunder's NPC-victim/boarder bins remain unmodelled
  (port models bins only on `PlayerInventory`), so the loot overlay reports only
  the credits share plus cargo plundered from a player victim.
- Remaining scope: mission-ship free-outfit arm and mass accounting in the ammo
  loop; the approach drive (state 0xd -> control 0xf -> timer expiry) depends on
  the partially ported state machine; the original's `DAT_00596d2a`
  clear-transient-sprites latch and `Frame_UpdateSpriteDistanceIntensity`
  distance dimming on pods are not modelled.

## 3. Open questions

- STR# 30000 entry 1 (the "unlicensed ship" note appended after the capture
  odds when the class is unlicensed) — pool not found in the archives dump;
  re-check whether any archive carries STR# 0x7530 and what its text is.
- Player death is not handled by the flight sim: a destroyed player can keep
  flying (armor <= 0, no game-over/destruction sequence). The board command
  correctly denies while destroyed, but the sim never ends the run.
