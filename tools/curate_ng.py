#!/usr/bin/env python3
"""Curated NovaGameplay -> struct/domain module mapping (name- and comment-driven).
Produces /tmp/ng_renames.json (list of addr/old/new rename records) for the batch
rename. Domain prefixes chosen to map 1:1 onto future module headers."""
import json
import re

named = json.load(open('/tmp/db_named.json'))
ng = [n for n in named if n['name'].startswith('NovaGameplay')]

PREFIX = {
    'Ship': 'Ship',
    'Weapon': 'Weapon',
    'Shot': 'Shot',
    'Outfit': 'Outfit',
    'Stellar': 'Stellar',
    'System': 'System',
    'Government': 'Government',
    'Mission': 'Mission',
    'Frame': 'Frame',
    'NovaUi': 'NovaUi',
    'ShipClass': 'ShipClass',
    'EncounterFleet': 'EncounterFleet',
    'Dude': 'Dude',
    'DataTables': 'DataTables',
}


def strip(n):
    return n.replace('NovaGameplay_', '', 1)


# ---- Explicit per-name overrides (addr -> domain). These win over regex. ----
OVERRIDE = {
    '0047d7f0': 'Ship',  # (placeholder)
}

# ---- Ordered (name-regex -> domain). First match wins. Most-specific first. ----
# NovaUi: all UI / windows / menus / buttons / cursor / splash / progress / starmap / shipyard-UI
UI = re.compile(
    r'Window|Menu$|Menu_|^Multiple|ThreeState|Button|HitTest|Starmap|Progress|'
    r'ProgressCallback|Splash|Cursor|SpecialInteraction|SpecialInteraction|'
    r'PromptString|StatusString|Modal|Shipyard|ListRow|CommWindow|Overlay$|'
    r'Overlay_|DrawSystemFactionConflictStatus|DrawCombatRankLabel|SearchDialog|'
    r'RunTargetShipComm|RunShipyard|Redraw|DrawThreeState|DrawPlayerSpecial|'
    r'HandlePlayerSpecial|RebuildSpecialInteraction|PollSpecialInteraction|'
    r'ShowCaptureDecisionDialog|RunBoardingPlunderWindow|DrawBoardingPlunderWindow|'
    r'TravelDestinationWindow|TravelDestinationMainWindow|TravelDestinationBribe|'
    r'TravelDestinationPaymentWindow|TravelDestinationServicesWindow|'
    r'TravelDestinationInteractionWindow|TravelDestinationInteractionLoop|'
    r'DrawTravelDestination|HandleTravelDestination|RenderTravelDestinationOverlay|'
    r'LoadTravelDestination|OutfitterMenu|OutfitterInteractionLoop|'
    r'HandleOutfitterMenu|RedrawOutfitter|IsOutfitterPurchaseAllowed|'
    r'HandleTravelServicesMenu|RedrawTravelServices|UpdateTravelSelectionOverlay|'
    r'UpdateTravelSelectionSprite|UpdateTravelTargetReticle|UpdateShipTargetReticle|'
    r'RunEscortShipManagement|DrawEscortShipManagement|HandleEscortShipManagement|'
    r'TargetShipCommWindow|RunPlayerSpecialInteraction|ShipyardHandle|ShipyardPage|'
    r'ShipyardSelect|ShipyardSetCursor|PollTravelScriptAction|PollMissionComputer',
    re.I)

# Weapon: banks / firing / turret / ammo / launch-bay weapon / weapon range/arc
WEAPON = re.compile(
    r'WeaponBank|SelectWeaponBank|SelectDirectFire|SelectGuided|SelectUnguided|'
    r'SelectGeneralWeaponBank|FirePlayerWeaponBank|FireShipWeapons|FireWeapon|'
    r'CanFireWeaponBank|CanWeaponHitTarget|WeaponCanTrackTarget|WeaponArc|'
    r'Ammo|LaunchBayWeapon|LoadedLaunchBay|GetWeaponFireInterval|GetWeaponBurst|'
    r'WeaponRange|MaxWeaponRange|TurretQuadrant|TurretTargetWithinArc|'
    r'ApplyTurretSpread|ClassifyShipWeapon|HasAnyFireableNonSecondaryWeapon|'
    r'InitShipWeaponBursts|HasMatchingWeaponAmmoCarried|SpawnShipFromCarrierBayWeapon|'
    r'SpawnWeaponImpactEffectPackage|SpawnWeaponImpactParticleBurst|'
    r'ResolveWeaponSplashImpact|ApplyWeaponOnHitEffects|GetShotImpactVariant',
    re.I)

# Shot / projectile: shots, beam queue, hit resolution, collision, splash impact
SHOT = re.compile(
    r'(?<!s)Shot|Projectile|BeamHit|BeamQueue|DrawBeamQueue|HandleShot|ResolveShot|'
    r'ResolveCollisions|QueueBeamHit|UpdateBeamHit|SpawnLinkedShots|'
    r'SpawnShotFromWeapon|SpawnAreaImpactEffects|SpawnImpactEffectSprite|'
    r'UpdateImpactEffectSprites|ResolveShipHitFromWeapon|SpawnDirectionalWeaponEffect|'
    r'UpdateDirectionalWeaponEffects|SpawnShotOnImpact|ShipDestructionDebris|'
    r'UpdateShotGuidance|AimStellarBatteryShot|ResolveShotCollisionHit',
    re.I)

# Mission: mission/misn slots, reactions, scripts, special ships
MISSION = re.compile(
    r'Mission|Misn|SpecialShip|Reaction|ReactionMission|MissionSlot|'
    r'TriggerReturnMission|ReplaceSubstringInMissionText|TryConsumeMission|'
    r'CheckMissionShip|RunMissionComputer|PollMissionComputer|'
    r'ExecuteReactionScript|CheckReactionCondition|AdvanceReactionSchedule|'
    r'ProcessInteractionReactionSlot|TickReactionSlots|TickShipInteraction|'
    r'ShowMissionShip|TrySpawnMissionShipAmbush|SpawnAmbientMissionShip|'
    r'SpawnMissionShip|SpawnSystemMisnShips|ResolveMissionFailure|'
    r'ResolveMissionSuccess|ResolveMisnSlot|ClearMisnSlot|FailMissionSlot|'
    r'PopulateMissionSlot|ActivateMission|EvaluateMissionLists|'
    r'RunMisnScriptPayload|TriggerReturnMission|SpawnRandomDudeMission|'
    r'MissionShipAnnouncement|AccumulatePlayerContributeMask',
    re.I)

# Outfit / economy
OUTFIT = re.compile(
    r'Outfit|Cargo|ShipyardAvailability|Purchase|Sale|Price|Inventory|'
    r'OwnedCount|OwnedItem|CanAfford|Payroll|Income|Refuel|Credits|'
    r'ComputePlayerCargo|ComputeRemainingCargo|RedistributeFleetCargo|'
    r'TransferCargo|ComputeFleetCargo|HasAnyOwnedItem|HasDisableOutfit|'
    r'PersistentDisableOutfit|DisableOutfitArmorThreshold|DisableOutfitShieldDrop|'
    r'DisableOutfitMode|DisableOutfitFlag|HasOutfitItemInPool|'
    r'HasLaunchEffectForSide|GravityShieldOutfit|GrantOutfitToPlayer|'
    r'RecomputeOutfitDerivedState|ReconcileOutfitPool|'
    r'RebuildWeaponBankPoolsFromOwnedOutfits|CountCarriedShipsForOutfit|'
    r'HasRequiredSpecialOutfit|RequireMask|ContributeMask|'
    r'ComputeOutfitPurchase|ComputeOutfitSale|ComputeOwnedOutfitResale|'
    r'ComputeScaledPurchasePrice|RebuildAvailableOutfitList|'
    r'ProcessFleetAutoTradeAtDestination|CollectStellarIncome|'
    r'ProcessPlayerEscortPayroll|SwapPlayerShipWithEscort|CanAffordAndCarryItem',
    re.I)

# Government / faction
GOVT = re.compile(
    r'Govt|Government|Faction|Reputation|PolicyFlag|Extortion|Alliance|'
    r'HostileOrXeno|ShareClass|IsCandidateHostileToTargeter|'
    r'PropagateHostilityFromAttack|SetSystemFillColor|'
    r'AreGovtsAllied|DoGovtsShareClass|GetSystemAlertLevel',
    re.I)

# System: visibility, map, discovery, regions, disasters
SYSTEM = re.compile(
    r'SystemVisibility|AdjacentSystem|SystemContaining|FindSystem|'
    r'FloodDiscover|ResolveSystemDiscovery|PlannedRoute|RegionTrigger|'
    r'SystemEvent|HasUsableTravelDestination|UpdateSystemAndStellarDisplay|'
    r'ResolveVisibleSystemForTravel|IsSystemVisible|InitRoamingShips|'
    r'UpdateDisasterStates|DoesSystemMatchMissionLocator|ShowSystemEventMessage|'
    r'SetSystemFillColor|CurrentSystemLinkHalfSpan|UpdateRandomEncounterCountdown',
    re.I)

# Encounter fleet
ENCFLEET = re.compile(
    r'EncounterFleet|RandomEncounter|FleetDef|SpawnShipFromFleetDef',
    re.I)

# Dude / roaming
DUDE = re.compile(
    r'Dude|RoamingShip|RoamingDude|RandomSystemDude',
    re.I)

# ShipClass
SHIPCLASS = re.compile(
    r'ShipClass|ClassAvailability|ClassSkill|FindLaunchBayShipClass|'
    r'SpawnEscortShipFromClass|LoadShipClassVisual|FindShipSpriteSetByResourceId',
    re.I)

# Stellar / travel
STELLAR = re.compile(
    r'Stellar|Spob|LandOnSpob|Hypergate|Hyperjump|Hyperspace|Travel|'
    r'Landing|Hyperdrive|TravelToSystem|TravelViaHypergate|ProcessTravelAndLanding|'
    r'TravelDays|JumpSequence|JumpDepth|SelectMissionStellar|'
    r'SelectMissionSystem|DoesSystemMatchMission|ResolveVisibleSystem|'
    r'ComputeTravelRangeSq|AdjacentTravelStellar|TravelFlagConnective|'
    r'IsStellarAdjacent|IsStellarActive|IsStellarValidRandomDestination|IsStellarUsable'
    r'StellarTargetsSpriteSet|GetStellar|UpdateStellarSprites|'
    r'TickStellarDefenseBatteries|TickStellarGravityPull|HandleShipStellarCrash|'
    r'ShipImmuneToStellarCrash|ShipHasGravityShield|ShipHasGravityShielding|'
    r'GravityShield|FindNearestAdjacentTravelStellar|FindReachableEmergencyDestination|'
    r'SetTravelDestination|RebuildTravelDestinationList|DrawTravelDestination'
    r'SelectRandomAdjacent|SelectRandomAdjacentDestination|CountValidLinkedStellar|'
    r'IsStellarActive|IsStellarValidRandomDestination|IsStellarUsable',
    re.I)

# Frame / simulation infra
FRAME = re.compile(
    r'SpaceflightLoop|TickSystems|FrameTiming|MeasureFrameTiming|ScreenFlash|'
    r'CombatChatter|Chatter|HyperspaceAudio|BlockingTransition|ElapsedTravelTime|'
    r'Viewport|PresentViewport|RestoreViewport|ScriptedManeuver|'
    r'SnapshotControlInputState|RestoreControlInputState|NavigationOverride|'
    r'StatModifier|AutoRepair|EscapePod|Recovery|UpdateFreeflightObject|'
    r'UpdateFadingEffect|UpdateScriptedManeuver|JitterPlayerStatModifiers|'
    r'RerollPlayerStatModifiers|RerollShipClassAvailability|'
    r'ResetNavigationOverrideLatch|UpdateEffectIntensityGlobal|'
    r'GetStatusEffectIntensity|UpdateSpriteDistanceIntensity|'
    r'RefreshTransitionFrameAuxVisuals|UpdateViewportWrapBackgroundSprites|'
    r'AnchorAuxHUDSprite|MeasureFrame|AddCombatRatingPoints|'
    r'ComputeShipStatusEffectDecayRate|IsRectVisibleInViewport|'
    r'TriggerSystemRegionEvents|CommitFrameAndLatch|FinishBlockingTransition|'
    r'RollProximityScanDetection|GetScannerStrength|ComputeShipJumpDepth|'
    r'GetJumpSequenceDurationMs|ComputeHyperspaceTravelDays|FormatElapsedTravelTime',
    re.I)

# Ship fallback (AI / movement / combat / targeting / disable / distress / boarding / escorts)
SHIP = re.compile(
    r'Ship|Ai|AI|Behavior|Target|Acquire|Hostile|Combat|Distress|Disable|'
    r'Board|Escort|Carrier|Steer|Throttle|Maneuver|Approach|Intercept|Pursue|'
    r'Attack|Formation|Wingman|Assist|Threat|Braking|LockedOn|Acquirable|'
    r'Heading|Speed|TurnShip|AlignHeading|AccelerateShip|MoveShip|'
    r'ShipsShare|ShipDamage|ShipHit|ShipFire|ShipDestroyed|ShipLost|'
    r'RecenterSpaceObjects|WorldWrapPosition|GetShipRadarColor|ShipTintColor|'
    r'ResolveShipTint|ShipFuelBurnRate|IsShipDestroyed|IsShipFireRestricted|'
    r'IsShipInAi|IsShipLockedOn|ShouldShip|PropagateLeader|'
    r'EnterShipAiState|EnterLeaderReturnState|LaunchEscort|LaunchShipFrom|'
    r'SpawnEscortShip|SelectNearestDisabledShip|SelectNearestEngaged|'
    r'SelectNearestHostile|ScoreAssistTarget|FindBestAssist|ResetShip|'
    r'DeactivateVacant|ShipsShareTargetLeaderChain|TurnShipTowardHeading|'
    r'SteerVelocityToward|AccelerateShipTowardPoint|UpdateShipAI|UpdateShipAi|',
    re.I)

# Data tables / init (goes last; only clear init)
DATATABLES = re.compile(
    r'InitGameplayDataTables|InitializeMainInterface|RunStartupSequence|'
    r'RunSpaceflightMode',
    re.I)

ORDERED = [
    (UI, 'NovaUi'),
    (WEAPON, 'Weapon'),
    (SHOT, 'Shot'),
    (MISSION, 'Mission'),
    (OUTFIT, 'Outfit'),
    (GOVT, 'Government'),
    (SYSTEM, 'System'),
    (ENCFLEET, 'EncounterFleet'),
    (DUDE, 'Dude'),
    (SHIPCLASS, 'ShipClass'),
    (STELLAR, 'Stellar'),
    (FRAME, 'Frame'),
    (SHIP, 'Ship'),
    (DATATABLES, 'DataTables'),
]


def assign(name, cmt):
    s = strip(name)
    for rx, domain in ORDERED:
        if rx.search(s + ' ' + name):
            return domain
    # comment fallback
    for rx, domain in ORDERED:
        if rx.search(cmt):
            return domain
    return None


def main():
    renames = []
    un = []
    for r in ng:
        dom = assign(r['name'], r['cmt'])
        if dom is None:
            un.append(r['addr'] + ' ' + r['name'])
            continue
        base = strip(r['name'])
        new = PREFIX[dom] + '_' + base.replace(PREFIX[dom] + '_', '', 1)
        # remove any leading module token already present
        renames.append({'addr': r['addr'], 'old': r['name'], 'new': new, 'domain': dom})
    json.dump(renames, open('/tmp/ng_renames.json', 'w'), indent=1)
    from collections import Counter
    c = Counter(r['domain'] for r in renames)
    print('renames:', len(renames), ' unassigned:', len(un))
    for d, n in c.most_common():
        print(f'  {d:16s} {n}')
    if un:
        print('UNASSIGNED:')
        for u in un: print('  ', u)
    # sanity: print samples
    print('\nsamples:')
    for r in renames[:15]:
        print(f"  {r['addr']} {r['old']} -> {r['new']} [{r['domain']}]")


if __name__ == '__main__':
    main()
