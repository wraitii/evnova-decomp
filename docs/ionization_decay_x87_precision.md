# Ionization decay: accepted double-vs-x87 precision divergence (0x0046c080)

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
