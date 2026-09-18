# Ionization: accepted double-vs-x87 precision divergences

Covers `Ship_ComputeIonizationDecayRate` (0x0046c080) and
`Ship_GetIonizationIntensity` (0x0046c160), plus the shared ionized-velocity
ramp constants.

`PlayerIonizationDecayFromOutfits` (`src/game/outfit.cpp`) accumulates the
ModType 39 (ion dissipator) bonus in `double`. The original evaluates the same
expression in x87 extended precision (64-bit significand). This is an accepted
platform-precision divergence: the port is not bit-exact here, and the
difference is not an original-game bug.

## Startup control-word evidence

- `PE_EntryPoint` (0x008713e0) jumps to `CRT_StartupMain` (0x00871160), the
  process entry path.
- `CRT_StartupMain` calls `FUN_00881060` at 0x0087123a (`FNINIT; RET`), which
  loads control word **0x037F**: extended precision (64-bit), round-to-nearest.
- No immediate `0x027F` (53-bit precision) exists in the image, and no
  `_controlfp`/`_control87` use was found. The only located `FLDCW`
  (0x004f4bba) temporarily changes the rounding mode for a truncating `FISTP`
  and restores the saved word.
- Uncertainty: a third-party DLL initialised later (for example QuickTime/qtml)
  could in principle change the control word at runtime. This cannot be
  resolved statically from the executable, and the SDL probe cannot inspect the
  original executable's x87 state.

## Concrete divergence

| input | original (x87 64-bit) | port (double) |
|---|---|---|
| base = 1.0F, ModVal = -100, owned = 1 | ~ -2.0817e-17 | 0.0 |

`(double)(-100) * 0.01` rounds to exactly `-1.0`; x87 extended retains the
double `0.01` offset (0.01_double = 0.010000000000000000208...), so adding
`1.0` leaves a tiny non-zero residual. Other values where `val * 0.01` loses
low bits can shift a later cancellation.

## Accepted scope

The cancellation example has a tiny absolute difference; no general ULP bound
or gameplay impact has been established. Software extended-precision emulation
is deliberately omitted under the accepted scope. Revisit only if bit-exact
reproduction is later required; that would need software 64-bit-significand
arithmetic (or an equivalent double-double emulation).

## `Ship_GetIonizationIntensity` capacity and division (0x0046c160)

The player ModType 40 (ion absorber) capacity scan adds each owned outfit's
`mod_val * owned` as an exact 32-bit integer product to the class
`ionization_capacity` (sign-extended int16), accumulating in x87 extended
precision. On the first (uncached) player call the original:

1. stores the **rounded float** total to `DAT_007356a8` (`FST float`), then
2. divides `ionization_points` by the **unrounded x87 accumulator** for that
   same call (`FDIV ST0,ST1` on the retained x87 value).

Subsequent calls read the rounded float cache and divide by it. The port
(`NovaOutfit_ComputeIonizationCapacity` / `NovaOutfit_GetIonizationIntensity`)
accumulates in `double` and always returns/divides by the rounded float, so its
first-call division can differ from the original by one rounding step. Accepted
platform-precision divergence under the same scope as the decay rate above; no
gameplay impact has been established. The comment on
`NovaOutfit_GetIonizationIntensity` records this inline.

## Ionized-velocity ramp constants

The post-decay ramp caps intensity with the double 0.7 (`DAT_00575478` NPC,
`DAT_00575668` player) and steps each axis by the double 0.025 (`DAT_00575480`
NPC, `DAT_00575670` player) times `g_avg_frame_tick_scale`. The port
(`NovaShip_UpdateIonizationCharge`) keeps these as `float` literals, the same
class of accepted precision divergence. The player caller also scales the decay
rate by a float tick scale rather than the original double
`g_avg_frame_tick_scale`.
