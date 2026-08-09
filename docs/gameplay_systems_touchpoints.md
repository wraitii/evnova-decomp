# EV Nova Gameplay Systems: Expected Areas and Current Touchpoints

This note is intentionally short, factual, and focused on in-flight gameplay systems and where they currently appear in the decompile.

## External mechanics baseline (for naming/orientation)

- Weapon behavior modes are data-driven (`wëap` guidance), including beam, homing, turret variants, point-defense, bomb/rocket, and carried-ship launch behavior.
- Mission objective families include destroy/disable/board/escort/observe/rescue/chase-off.
- Game data is plugin-overridable (`Data` + `Nova Plugins` layering); gameplay behavior is strongly resource-driven.

References:
- https://evn.fandom.com/wiki/W%C3%ABap
- https://escape-velocity.games/EVN_Walkthroughs/html/index.html
- https://escape-velocity.games/docs

## In-flight runtime entrypoints identified

- `0x00417600` `NovaGameplay_SpaceflightLoop`
- `0x004186b0` `NovaGameplay_TickSystems(char run_full_tick)`
- `0x00433050` `NovaGameplay_HandleShip(int ship)`
- `0x00435830` `NovaGameplay_HandleShot(int * shot)`
- `0x00437e20` `NovaGameplay_ResolveCollisions()`

## Frame phase map (from scope labels)

`NovaGameplay_SpaceflightLoop` names these scope IDs and therefore execution buckets:

- `4` HandleShip
- `5` HandleShipDisplay
- `6` AI routines
- `7` HandleShot
- `8` misc handlers
- `9` collisions
- `10` player
- `12` DrawStatus
- `13` ProcessSpriteWorld
- `14` AnimateSpriteWorld
- `15` PostAnimate
- `16` locking
- `17` erase
- `18` draw
- `19` update
- `20` calc AI odds

Scope helpers:

- `0x0046fde0` `ProfileScope_SetLabel(short scope_id, char * label)`
- `0x0046fe20` `ProfileScope_Begin(short scope_id)`
- `0x0046fe80` `ProfileScope_End(short scope_id)`

## AI / movement / firing chain currently mapped

Primary chain inside `NovaGameplay_TickSystems`:

- `0x00401000` `NovaGameplay_UpdateShipAI(int * ship, char skip_heavy_ai)`
- `0x00405590` `NovaGameplay_UpdateShipAiState(int ship)`
- `0x00408150` `NovaGameplay_ApplyShipAiControls(int * ship)`
- `0x00414550` `NovaGameplay_FireShipWeapons(int ship)`

Observed responsibilities:

- `UpdateShipAI`: per-ship high-level AI decision dispatch and state transitions.
- `UpdateShipAiState`: AI mode/state machine evolution (maneuver/mode selection).
- `ApplyShipAiControls`: converts selected AI mode into heading/throttle/velocity control outputs.
- `FireShipWeapons`: weapon-bank readiness checks, fire dispatch, ammo/energy side effects.

## Projectile/weapon behavior touchpoints

- `NovaGameplay_HandleShot` contains strong `wëap`-aligned behavior branches by weapon-type fields (including turreted, splash/aoe, carried-ship behavior).
- `NovaGameplay_FireShipWeapons` manages bank cadence and per-bank resource usage arrays.
- `NovaGameplay_ResolveCollisions` is the shared hit-resolution pass after movement/shot updates.

### Missile seek / jamming subsystem (mode 1 homing)

- `WeaponDef.jam_vuln_1..4` (+0xBA) seed the four `ShotState.lock_quality_0..3` seek channels at spawn: `lock_quality[i] = 0` if `jam_vuln[i]<1` else `Random(0..jam_vuln[i])`.
- `ShipState.jamming_score_1..4` (+0xC926) = owning-govt `InhJam1-4` + outfit jamming opcodes `0x21..0x24`, halved if govt flag 0x80, clamp 0-100.
- `NovaGameplay_GetShipJammingScore(ship, ch)` (0x00464810) lazily computes those scores.
- Lock gate in `NovaGameplay_UpdateShotGuidance`: usable lock requires `lock_quality[i] > 100 - jamming_score[i]`; higher jamming → missile loses lock / flies straight. (See `docs/weapon_mode_code_mapping.md`.)

## Simulation-tick misc-handler subsystems mapped (scope 8 / 9 / 0xb / 0xc)

Broad-phase collision (scope 9):
- `0x004772d0` `NovaGameplay_TestSpriteLayerOverlaps` — pairwise AABB overlap test between two SpriteLayers; on overlap invokes the per-entity collision callback (`+0x94`) with both entities + overlap rect. Sourced from SpriteLayer.c. TickSystems uses it to test shot containers and freeflight objects against each other and the sector.

Stellar environment (scope 8):
- `0x0042d890` `NovaGameplay_TickStellarDefenseBatteries` — per-stellar defense-battery fire: ticks the engage countdown, scans nearest hostile target (squared-distance + `NovaGameplay_IsCandidateHostileToTargeter`), spawns charged ShotState entities. Skipped when time frozen.
- `0x0043adb0` `NovaGameplay_TickStellarGravityPull` — pulls unshielded active ships toward each stellar gravity well; sets `g_gravity_pull_active`.
- `0x0043aed0` `NovaGameplay_HandleShipStellarCrash` — pixel-mask tests ships against body sprite; on contact kills the ship, resets player target, spawns impact effects.
- `0x0046df70` `NovaGameplay_ShipHasGravityShieldOutfit`, `0x0046e120` `NovaGameplay_ShipHasGravityShielding`, `0x0046e210` `NovaGameplay_ShipImmuneToStellarCrash` — the gravity/body-immunity gate family (outfits 0x26/0x29/0x2a + class flags).
- `0x0046e2f0` `NovaGameplay_AccelerateShipTowardPoint(ship_pos,target_pos,max_accel,velocity_xy)` — steering/force application toward a point.

AI inbound threat / evasion (scope 0xb):
- `0x00422210` `NovaGameplay_TallyInboundWeaponThreat` — sums damage of shots targeting each ship into `ShipState.inbound_weapon_threat` (+0xC8CE), reset each full tick.
- `0x004221d0` `NovaGameplay_IsInboundThreatExceedingDefenses` — gates AI reaction (evasion / guided-fire choice) based on the threat vs defensive budget.

Scan detection / radar (scope 0xc):
- `0x0045d030` `NovaGameplay_RollProximityScanDetection` — rolls detection odds from system scan-bonus minus player scanner strength; latches `g_proximity_scan_detected`.
- `0x0046abb0` `NovaGameplay_GetScannerStrength` — summed scan quality of owned scanner outfits (id 0x18).
- `g_proximity_scan_detected` (0x007caba0) drives radar blip drawing in `NovaUi_DrawStellarRadarPanel`.

Travel / interaction UI:
- `0x00443760` `NovaGameplay_TickShipInteractionReactions` — per-tick loop over 16 interaction slots; per-slot handler composes feedback, mission resolution, surrender/board conditions.
- `0x004444f0` `NovaGameplay_BuildTravelDestinationDescription` — builds destination description text (stellar/system resolution + localization).
- `0x0042cc30` `NovaGameplay_TickTravelCountdownSprite` (`DAT_00734c18` countdown) — flashes the travel/limbo countdown sprite; armed at 30 in escape-pod seq, cleared on travel completion.
- `0x0042cbb0` `NovaGameplay_AnchorAuxHUDSpriteToOrigin`.

Player ship targeting / selection (reconstructed in `src/game/targeting.cpp`, wired in `NovaFrame_SpaceflightLoop`):
- `0x00461bd0` / `0x00461f60` `Ship_FindNextPlayerCycleTarget` / `_Previous` — backquote (`) / Shift+backquote cycle the player's primary target ship; the held include-combat modifier (Alt or the original's 'k' / 0x6b) restricts the cycle to combat-relevant ships (targeting the player or a player-targeting ship). Ported as `NovaTargeting_FindNext/PreviousPlayerCycleTarget`.
- `0x00462bd0` `Ship_SelectNearestHostileCombatTarget` ('o') and `0x00462850` `Ship_SelectNearestEngagedTarget` (Alt+'o') — nearest combat-target scans, ported as `NovaTargeting_SelectNearest*`.
- `0x0040faa0` `Ship_IsShipAcquirableAsTarget` — pairwise acquisition predicate (player + NPC branches), ported as `NovaTargeting_IsShipAcquirableAsTarget`.
- `0x0046c7a0` `Ship_CheckShipDisableThresholdState`, `0x0040f6d0` `Ship_IsShipEligibleForDistressCall`, the two cloak-scanner outfit predicates (`0x0046c930` / `0x0046ca60`) — the shared eligibility helpers behind all of the above.
- Click-to-target ship picking (`SpaceflightView::PickShipAt`, sprite half-span hit test approximating the original's pixel-test pass) sets the primary target directly (the manual: "click on a ship to select it with your targeting sensors").
- `0x0042ede0` `NovaUi_UpdateShipTargetReticle` — 4-corner bracket reticle around the primary target, drawn as SDL brackets with the state-encoded frame mapping (0xc fire-restricted grey / 0x8 targeting-player green / 0x0 distress yellow / 0x4 other white) and the decaying 256→0 pulse (60/sec). Real bracket sprite frames are TODO(decomp).

Weapon aiming / lead subsystem (shared by ApplyShipAiControls, SpawnShotFromWeapon, turret evaluators, stellar batteries):
- `0x004eed20` `Math_Sqrt`
- `0x004619b0` `Math_AngleFromVector2D` — atan2 to EV game-degree angle; core behind `Math_BearingFromPointToPoint`.
- `0x0043b670` `Math_BearingFromPointToPoint`
- `0x0043b740` `NovaGameplay_AimWeaponPredictive(ship,target_ship,weapon_id,ship_pos_xy)`, `0x0043b8c0` `NovaGameplay_AimWeaponLeadVelocity(ship,target_pos_xy,target_vel_xy,weapon_id,ship_pos_xy)`, `0x0043ba30` `NovaGameplay_AimStellarBatteryShot(battery_stellar,target_ship)` — target-lead/aim computations using projectile speed; handle weapon_mode_code 6 fast-projectile math.

> **Important:** all three return `short` (the predicted aim bearing in EAX), NOT void. The original void decomp was a Ghidra return-model bug: the final `Math_BearingFromPointToPoint` returns in EAX which the void prototype ignored. Callers assign the returned bearing to the ship desired-heading field (+0x1A) or a shot heading (+0x20), and then apply `shot_random_spread` rounding. Signature fixed in the DB.

Globals touched this session:
- `g_gravity_pull_active` (0x0073548c) — set when the current system contains an active nonzero-gravity stellar. The player-control path uses it to suppress the afterburner's boosted-cap branch.
- `g_player_in_gravity_well` (0x007cab1b; **misnamed in the current Ghidra DB**) — player afterburner-active latch. `Ship_HandlePlayerShipControl` sets it only when the opcode-15 command is held, fuel is positive, and `Ship_GetShipFuelBurnRate` is affordable; while set it burns that rate each frame. It does not mean that the player is physically inside a gravity well.
- `g_navigation_override_done` (0x007354ab) — navigation-override latch cleared at start of each player tick.

## Struct and field touchpoints already set

`ShipState` updates applied:

- `0xC8CE` `short inbound_weapon_threat` — accumulated inbound shot damage aimed at this ship; reset each tick, gated vs defenses for evasion/guided-fire.
- `0xC930` `char * license_name` (inferred)
- `0xC938` `undefined8 license_seed` (inferred)
- `0xC940` `int license_token` (inferred)

AI/combat-relevant offsets repeatedly used and next to formalize:

- `0xC8C8` AI state code (provisional)
- `0xC8CA` AI control mode code (provisional)

## AI state map (`ShipState.ai_state_code` @ 0xC8C8)

Traced from the handlers in `NovaGameplay_UpdateShipAiState` (0x00405590) and its helpers. Confidence: high on states 0-3/4/5/0xb-0x16; provisional on fine distinctions between 2/3/4 and 0xd/0xf. Doc comments on 0x00405590 (full table), 0x00411270 (idle set), 0x00415e60 (0x10).

| code | meaning |
|---|---|
| 0 | idle / track-parked |
| 1 | travel to system (`ai_secondary_target_slot` = stellar, steers to map coords) |
| 2 | idle-template / approach station-keeping |
| 3 | attack target ship (`primary_target_ship_slot`) w/ disable-pressure |
| 4 | attack target w/ mutual-target & allied-govt chain exclusions |
| 5 | pursue/attack `ai_target_ship_slot` (chase control 0xb) |
| 6 | follow/hold (control 1); entered when jump can't initiate in combat |
| 7 | escort/follow primary at range (control 9), escort-arrive |
| 8 | disengage cleanup / stationary (control 10) |
| 9 | escort-pursue primary (chase 0xb) |
| 10 | assist/reaction behavior (from `UpdateShipAssistResponseBehavior`) |
| 0xb | hold-station / follow target (waits `ai_station_hold_timer`), entry to 5 jump if behavior 5 |
| 0xc | engage target at turn radius w/ disable-pressure |
| 0xd | acquire disabled/boardable target (board state) |
| 0xe | drift/evade (adds polar velocity) |
| 0xf | disabled-pursue/flee at turn radius (gravity-shield scales range) |
| 0x10 | scripted/invulnerable maneuver — unhittable (seen in `CanWeaponHitTarget` + `UpdateBeamHitQueue`), steers to `g_scripted_maneuver_state_ptr` target |
| 0x11 | static hold (control 0x15) |
| 0x12 | attack a stellar system (`ai_secondary_target_slot`) w/ weapon banks |
| 0x14 | traveling/jumping to a system (sets `jump_destination_stellar_id`, propagates to escorting ships) |
| 0x15 | disengage (clears target, drops to 8) |
| 0x16 | defunct (clears targets) |

Idle / non-combat set (used by `NovaGameplay_IsShipInNonIdleAiState` 0x00411270): `0, 1, 2, 7, 0x14`. Note `0x14` (jump/travel) is in the idle set even though it is an active travel state; the predicate is used to test whether a target ship is *fighting*, not whether it's moving.
- `0xC906`, `0xC908`, `0xC92E` target/formation/relationship state (provisional)
- weapon-bank arrays around `+0xC8`, `+0xD0`, `+0xF8` patterns in ship-local bank loops

## Non-gameplay integrity path currently interleaved with gameplay

Runtime integrity/license helpers are active in both bootstrap and spaceflight loops:

- `0x00416090` `LicenseDigest_ScaleBySessionByte`
- `0x004160a0` `LicenseDigest_AccumulateByte`
- `0x004160b0` `Rotl8_BySelf`
- `0x004160e0` `LicenseDigest_TouchAndLoadByte`

These should remain separated from gameplay naming decisions except where they directly alter gameplay state.
