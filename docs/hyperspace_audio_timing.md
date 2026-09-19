# Hyperspace sound duration and Mac display fade

The port's reciprocal SDL playback rate is incorrect. The original reciprocal
is a **duration scale**, not a playback-speed multiplier.

## Mixer evidence (Windows Ghidra)

- `0x0046ab00` writes `trunc(65536 / ShipClassDef[+0x44])` into
  staged sound descriptor `+8`. The double at `0x00575818` is 65536.
- `Audio_AllocateVoiceSlot` (`0x004d6550`) computes the fixed-point ratio
  `output_rate / source_rate` through `0x00507d96`, then multiplies it by
  descriptor `+8` through `0x00507d16`.
- `0x004d6b90` passes that result shifted right four to `0x00507f90`.
- `0x00507f90` decodes one source sample, adds that increment to an
  accumulator, and emits the change in its integer part as output frames.
  The increment therefore measures output frames per source sample.
  A smaller reciprocal produces fewer output frames and a shorter sound.

SDL's source sample rate must consequently be multiplied by the ship class
multiplier itself. This is also consistent with the Bible's flags: bit 0 is
slow jumping, bit 1 semi-fast, and bit 2 fast. The port's reciprocal makes
those flags behave in reverse.

The bundled `Nova Sounds.rez` snd 128, "Warp up", has 2094 mono IMA4 packets
of 64 samples at 22050 Hz: 134016 samples, or 6.077823 seconds. Header and
packet layout agree with the ResForge Sound editor definitions and the port
decoder. These are ideal cue durations; original mixer quantization, polling,
braking, alignment, and output buffering can affect observed timings.

| Class multiplier | Current port cue | Correct cue |
| --- | ---: | ---: |
| 0.91 (slow) | 5.53 s | 6.68 s |
| 1.30 (normal) | 7.90 s | 4.68 s |
| 1.69 (semi-fast) | 10.27 s | 3.60 s |
| 2.08 (fast) | 12.64 s | 2.92 s |

The stock Shuttle (shp resource 128) flags are `0x0120`, selecting the
normal 1.30 multiplier. These durations begin when the warp-up cue starts,
not when the player first requests a jump.

## Mac fade evidence

In the i386 slice of the original Mac executable
(`/Applications/EV Nova.app/Contents/MacOS/Ev Nova.original`),
`_FadeWhiteIn` (`0x546f`) and `_FadeWhiteOut`
(`0x54d1`) each issue a single asynchronous `CGDisplayFade` with the immediate
float 1.5 (`0x3fc00000`). They fade between 0 and 1, with white RGB.
`_HandlePlayer` at `0x683f1..0x68404` starts the fade only when the scalar is
positive and the fade-active flag is clear. `0x6840b..0x6841f` resets the
scalar and starts fade-out after the hold ends. The scalar is a trigger,
not continuously sampled opacity.

The Mac `_AdjustWarpSoundSpeed` (`0x5ecc`) also writes a reciprocal into the
sound descriptor. The bundled `AmbrosiaTools.framework/Versions/A/AmbrosiaTools`
i386 implementation independently confirms its meaning: `_ST_PlaySoundParam`
(`0xe3dc2`, ratio calculation at `0xe3fa3..0xe3fcd`) computes
`output_rate/source_rate * descriptor[+8]`. `__ST_MixerDispatch` (`0xe486e`)
passes the result shifted right four to `_ADPCM_Mixer` (`0xe6e5a`). The latter
adds this increment per decoded sample (`0xe6ff9`) and emits the change in
the accumulator's integer part. Mac and Windows use the same duration-scale
convention. The exact seconds above use the bundled Windows sound resource.

Using the port's current progress formula and the Mac threshold of 55,
the normal Shuttle begins fading at about 3.82 s. With corrected audio, its
boom occurs at about 4.68 s, before a 1.5 s fade-in completes. The report's
predicted 2.6 s full-white hold depends on the inverted sound duration.
The Mac executable's constants at `0xdd968` and `0xdd67c` are 55.0 and 5.0,
confirming the trigger expression `(progress - 55) * 5`.
Arrival calls fade-out with a starting blend of 1; preserving that behavior
may still produce a jump in opacity when arrival interrupts fade-in.

For a normal Shuttle, approximately 0.85 s elapses between fade trigger and
cue completion. A linear 1.5 s fade would therefore be about 57% complete.
The executable does not wait for fade-in to finish: `_FadeWhiteOut` supplies
an explicit start blend of 1.0 and end blend of 0.0 over 1.5 s. Thus the
requested sequence is partial fade-in, full-white start of fade-out, then
reveal. The exact visual response to overlapping asynchronous fades remains
an OS behavior to validate; these are the arguments and ordering in the binary.
The hold-end fade call occurs on the following player update, so system-load
time and update cadence can also change the visible peak.

## Unused Windows ramp

Windows computes `progress * 0.3 - 15` at `0x004505e0..0x00450601` (verified
doubles at `0x00575628` and `0x00575630`). It clamps the result to [0,100]
and calls `0x00467e60` each hold update; that function is a bare return.
Unlike Mac, this call site has no fade-active one-shot latch.

For a normal Shuttle, with the same progress clock and corrected sound rate:

- The scalar becomes positive at progress 50, about 3.59 s after cue start.
- At cue end, about 4.68 s, progress is about 73.3 and the scalar about 7.0.
- Reaching scalar 100 requires progress 383.33, about 19.14 s, which the
  normal cue-gated jump never reaches.

These are scalar values, not demonstrated Windows opacity: the implementation
does not render them. At hold end it calls `Anim_EaseFrameProgress(100, 0)`
(`0x00467d60`), which immediately stores a fixed-point value of 1.0; it does
not run a 1.5 s display fade. The unused Windows code consequently supplies
no evidence that the Mac fade must complete before arrival.

Investigation only: gameplay code has not been changed. Correcting the SDL
rate and implementing the Mac one-shot fade are corrections to the port;
they are not evidence of an original gameplay bug.
