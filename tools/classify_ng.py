#!/usr/bin/env python3
"""Struct/domain-first categorization of NovaGameplay_* functions (scratch)."""
import json
from collections import Counter

named = json.load(open('/tmp/db_named.json'))
ng = [n for n in named if n['name'].startswith('NovaGameplay')]

hand = {
    'AccelerateShipTowardPoint': 'Ship-sim',
    'FindShipSpriteSetByResourceId': 'DataTables/Init',
    'FloodDiscoverAdjacentSystems': 'System',
    'FormatLocalizedCountWord': 'DataTables/Init',
    'GetShipFuelBurnRate': 'Ship-sim',
    'HandlePlayerHyperspaceSequence': 'Stellar/Travel',
    'NormalizePlannedRouteToCurrentSystem': 'System',
    'PropagateLeaderRetreatStateToFollowers': 'Ship-sim',
    'RerollPlayerStatModifiers': 'Frame/infra',
    'ResolveSystemDiscoverySlot': 'System',
    'UpdatePlayerShipControl': 'Ship-sim',
    'WorldWrapPositionRelativeToPlayer': 'Ship-sim',
    'TrySpawnRandomEncounterFleet': 'EncounterFleet',
    'SpawnRandomEncounterFleet': 'EncounterFleet',
    'GetWeaponFireIntervalTicks': 'Weapon',
}

# (label, keywords) -- checked in order; most-specific domains first.
# NOTE: keyword lists must be FULL for every domain, otherwise the residual
# "Ship-sim" rule over-catches. Keep all domains populated.
RULES = [
    ('UI', ['Window', 'Menu', 'ThreeState', 'three-state', 'Button',
            'SpecialInteraction', 'special interaction', 'Cursor',
            'HitTestAndTrack', 'PromptString', 'StatusString', 'Modal',
            'Starmap', 'Progress', 'Splash', 'political overlay', 'Shipyard']),
    ('Shot/Projectile', ['shot', 'Shot', 'projectile', 'beam', 'homing',
                         'bolt', 'directional weapon', 'SpawnShot',
                         'SpawnLinked', 'Explosion', 'SpawnImpact',
                         'ImpactEffect', 'WeaponSmoke', 'BeamHit',
                         'BeamQueue', 'DrawBeam']),
    ('Weapon', ['WeaponBank', 'weapon bank', 'turret', 'ammo', 'launch-bay',
                'guided', 'fire interval', 'FireWeapon', 'FireShipWeapons',
                'FirePlayerWeapon', 'Burst', 'armory', 'WeaponArc',
                'CanFireWeaponBank', 'TurretQuadrant', 'TurretSpread',
                'WeaponBurst', 'weapon fire']),
    ('Outfit/Economy', ['outfit', 'Outfit', 'cargo', 'Cargo', 'equipment',
                        'purchase', 'sale', 'inventory', 'RequireMask',
                        'ContributeMask', 'player loadout', 'economy',
                        'price', 'OwnedCount', 'fleet cargo', 'CanAfford',
                        'OwnedItem', 'outfit pool', 'equipment station']),
    ('Stellar/Travel', ['stellar', 'Stellar', 'spob', 'land', 'Land',
                        'hypergate', 'travel destination', 'Travel',
                        'travel', 'hyperspace sequence', 'interstellar',
                        'LandsOn', 'hyperjump', 'TravelDays',
                        'JumpSequence', 'JumpDepth', 'MoveToSystem']),
    ('System', ['system visibility', 'visibility map', 'linked system',
                'current system', 'SystemEvent', 'region trigger',
                'neighbor system', 'system region', 'FloodDiscover',
                'ResolveSystemDiscovery', 'PlannedRoute',
                'SystemContainingStellar']),
    ('Government/Faction', ['govt', 'Govt', 'government', 'Government',
                            'faction', 'Faction', 'alliance', 'reputation',
                            'policy', 'CommAid', 'aid', 'hostility',
                            'Extortion', 'ReputationCredit',
                            'IsReputationTracking']),
    ('Mission/Misn', ['mission', 'Mission', 'misn', 'Misn', 'mission fleet',
                      'MissionTarget', 'special ship', 'specialShip',
                      'interaction slot', 'misn slot', 'Reaction',
                      'schedule']),
    ('ShipClass', ['ship class', 'ShipClass', 'class availability',
                   'ship types', 'class skill', 'ship type',
                   'availability list', 'ShipClassVisual',
                   'ShipClassSkill']),
    ('Dude/NPC', ['dude', 'DudeDef', 'random Dude', 'roaming',
                  'encounter ship']),
    ('EncounterFleet', ['encounter fleet', 'FleetDef', 'fleet def',
                        'random encounter']),
    ('Freeflight', ['freeflight', 'Freeflight', 'container', 'debris']),
    ('DataTables/Init', ['DataTables', 'data tables',
                         'InitGameplayDataTables', 'InitializeMainInterface',
                         'InitGameplay', 'StartupSequence',
                         'InitializeGameplay', 'ShipSpriteSet']),
    ('Frame/infra', ['SpaceflightLoop', 'TickSystems', 'ScreenFlash',
                     'screen flash', 'effect intensity', 'EffectIntensity',
                     'Chatter', 'HyperspaceAudio', 'BlockingTransition',
                     'ElapsedTravelTime', 'EngagementProgress',
                     'ScriptedManeuver', 'FrameTiming', 'frame timing',
                     'MeasureFrame', 'profile scope', 'Viewport', 'Present',
                     'RestorePending', 'RestoreViewport',
                     'RenderTravelDestinationOverlay', 'SnapshotControl',
                     'RestoreControl', 'NavigationOverride', 'Measure',
                     'StatModifier']),
    ('Ship-sim', ['Ai', 'AI', 'ai_', 'target', 'Target', 'hostile',
                  'Hostile', 'combat', 'Combat', 'steer', 'maneuver',
                  'throttle', 'Disable', 'Distress', 'Boarding', 'Board',
                  'escort', 'Escort', 'carrier', 'carried', 'approach',
                  'attack', 'pursue', 'docked', 'Missile seek', 'Thrust',
                  'Speed', 'movement', 'Movement', 'align', 'Eligible',
                  'Response', 'assist', 'pressure', 'braking', 'acquire',
                  'attackers', 'fire restriction', 'ShipFire', 'ShipDestroyed',
                  'RadarColor', 'TintColor', 'SteerVelocity', 'TurnShip',
                  'MaxShield', 'MaxArmor', 'FuelCap', 'RegenRate',
                  'RechargeRate', 'StatusEffect', 'ScannerStrength',
                  'GravityShield', 'GravityShielding', 'FireRestricted',
                  'Destroyed', 'PriorityThreat', 'heading', 'CurrentMass',
                  'ShipTotalMass', 'AutoRepair', 'Missile',
                  'DisablePressure', 'Escape', 'Recovery', 'Threat',
                  'LockedOn']),
]


def classify(r):
    n = r['name']
    short = n.replace('NovaGameplay_', '', 1)
    if short in hand:
        return hand[short]
    text = (n + ' ' + r['cmt']).lower()
    for label, kws in RULES:
        if any(k.lower() in text for k in kws):
            return label
    return None


def main():
    c = Counter(classify(r) for r in ng)
    order = ['Ship-sim', 'Shot/Projectile', 'Weapon', 'Outfit/Economy',
             'Stellar/Travel', 'System', 'Government/Faction', 'Mission/Misn',
             'ShipClass', 'Dude/NPC', 'EncounterFleet', 'Freeflight',
             'DataTables/Init', 'Frame/infra', 'UI']
    tot = 0
    for label in order:
        n = c.get(label, 0)
        print('%-45s %3d' % (label, n))
        tot += n
    print('-' * 55)
    print('%-45s %3d  | None=%d  | total=%d' %
          ('TOTAL assigned', tot, c.get(None, 0), len(ng)))


if __name__ == '__main__':
    main()
