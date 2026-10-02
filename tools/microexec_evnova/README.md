# microexec: EV Nova project

The EV Nova integration for the standalone engine in `../microexec/`. When
invoked from this repo, the engine discovers `microexec.toml` here as the unique
profile under `tools/`. Its paths resolve from the repo root (`root = "../.."`),
and it points at:

- `scenario.py`: a data plugin over the shipped `Nova Data *.rez` archives
  (via `tools/rez_extract.py`). The original copies resource payloads into
  fixed-size tables (`g_outfit_defs`, ...). The port's decoders in
  `src/game/scenario_data.cpp` name each field's payload offset, so a Ghidra
  field decoded under the same name gets `scenario_observed`: its distribution
  across every shipped record. Evidence kinds: `scenario_loader` (a family's
  decoder), `scenario_stats` (one field over all records) and
  `scenario_resource` (raw payload bytes).
- `rules.json`: bridge rules from Ghidra slot keys to the port's `GameState`
  paths, for the port comparison.
- `host/`: the native port host, `evnova_microexec_host`, which links
  `evnova_runtime` unchanged. Its field and target tables are generated at
  build time by `host/gen_reflection.py` from clang's JSON AST of the
  `src/game` headers.

The toml also sets the prompt wording (a mid-game session of the stock
scenario, 0-based resource ids, player at `g_ship_states[0]`), the trackers
(`decomp-progress.tsv` / `decomp-skipped.tsv`), `NovaRandom_Range` as the RNG,
the `docs/` reference search (Bible, ResForge), and the `@port` / `// Ghidra`
citation markers.

Status: the original side works end to end on `Ship_ComputeTradeInValue`,
`Outfit_ComputeScaledPurchasePrice` and `Ship_FindNearestHittableWeaponTarget`.

## Setup

From the EV Nova repo root:

```sh
python3 -m venv tools/.venv
tools/.venv/bin/pip install -r ../microexec/requirements.txt
```

The engine reads credentials from `../microexec/.env`. From outside this repo,
pass `--project /path/to/EVNova-decomp/tools/microexec_evnova/microexec.toml`
before the command.

## Port comparison

```sh
cmake build/release -DEVNOVA_MICROEXEC=ON && cmake --build build/release --target evnova_microexec_host
tools/.venv/bin/python ../microexec/microexec.py diff 0x0046bc90
```

The host generates everything it needs from the port itself:

- every public data member of every `game::` struct is settable by path from
  `GameState` (`scenario.governments[3].classes[0]`, `ships_[1].is_active`);
  a `std::vector` table is sized with `size`, a `std::array` checks the extent.
- every namespace-level function declared in a `src/game` header and cited for
  an original address (`// @port 0x...`, else the first `// Ghidra 0x...` in
  the comment block above it) is a target. `reflect.hpp` supplies each
  parameter from its type: `GameState &` / `const ScenarioData &` from the
  populated state, a record (`const Ship &`) built fresh from the
  `<param>.<path>` arguments, a scalar from its argument, or `std::string` / `std::string_view` from a byte
  string argument (by value or const reference). Signatures it cannot
  drive (pointers, out-parameters, non-scalar returns) are listed as `unsupported`.
  A void function is called and compared on its side effects alone.

`echo describe | evnova_microexec_host` prints both. So a new port field or a
new cited function needs no host code, and `run` compares a target
automatically once the host is rebuilt.

`globals` rules map a table to its port path and fix its extent: for example,
`g_government_defs` maps to `scenario.governments` with `count` 256, because the
port range-checks against its vector size. With `"auto": true` the rest is
inferred against the host's records: same-named fields, numbered fields into
arrays (`class_1` -> `classes[0]`), and arguments by name or position. Only the
exceptions are written down: a rename (`base_cost` -> `cost`), a `skip`, an
`unmapped` reason, or a `targets` override.

The system and stellar tables are mapped for `0x0040c790`. System nav ids
use `add: 128` to convert original indexes to port resource ids; `preserve: [-1]`
keeps empty slots empty. The target's `return` rule applies `add: -128` with the
same sentinel preservation before comparing returns. Field offset rules also
apply to side effect comparisons.

### String arguments

The host advertises narrow string parameters as `string`. The bridge pairs
an original `char *` (also signed/unsigned char) with that kind by name or
position, then reconstructs the input through its first NUL from recorded
initial bytes. It sends `arg-bytes <name> <hex>`; `-` represents an empty
string. Hex preserves whitespace, non-UTF-8 bytes, and protocol boundaries.
The host owns the bytes through the call and constructs string views only
from the final parameter storage, after movement into the call tuple.

Missing/conflicting bytes, null pointers, unproven terminators, strings over
65,535 bytes, and original writes to the input produce `not-comparable`.
Existing recordings without enough bytes must be rerun. Wildcard cases that
force every character nonzero have no terminator; the generic per-run
`wall_time_budget` (default 5 seconds) bounds their execution and reports a
timeout. Suite logs show each case before it starts. Mutable string
references and string returns remain unsupported; string contents are not
included in scalar access tracing. This supports string parameters, rather
than translating character-array fields into string members of records.

`CString_Length` is explicitly emulated so comparisons use the length of
the actual input. Expression/script scratch-buffer writes are skipped because
the port uses local strings. Evaluator gameplay reads and writes still need
ordinary bridge rules; unmapped state is reported rather than defaulted.

### Side effects and access tracing

Besides the return value, the host reports what the port did to the
`GameState` (protocol in `host/main.cpp`): `write <path> <final> <initial>`
for every scalar the port stored to or changed, and `read-unset <path>` for
every scalar it read before writing that the case never `set` (it saw the
default-constructed value, so a `match` there proves little). The bridge
compares the writes with the original run's (`../microexec/README.md`,
"Side effects").

Reads and stores are seen through clang SanitizerCoverage
(`-fsanitize-coverage=func,trace-loads,trace-stores`): with
`EVNOVA_MICROEXEC_TRACE=ON` (the default under `EVNOVA_MICROEXEC`) the host
links `evnova_microexec_port`, a private instrumented recompile of the
`evnova_runtime` sources, so `evnova_runtime` and the game are built as usual.
Just before the call the host maps every scalar reachable from the root
(generated `Collect`, including `std::array` / `std::vector` elements); the
hooks (`host/trace.cpp`, no-ops outside the call window) mark leaves read or
written. After the call the state is re-walked by path, so a vector the port
reallocated is still compared; `-DEVNOVA_MICROEXEC_TRACE=OFF` builds the host
against `evnova_runtime` and reports writes only (`trace snapshot` in
`describe`), found by comparing bytes before and after.

Limits: loads and stores inside library calls (`memcpy`, `std::fill`) and
accesses wider than the scalar they hit (struct copies) are not seen by the
hooks (writes are still found by the byte comparison; such reads are not);
elements a resize appends have no pre-call address and are ignored;
`std::vector<bool>`, pointers, strings and maps are not mapped.

You can replay a case by hand:

```sh
printf 'case 1 0x0046bc90\nsize scenario.governments 256\nset scenario.governments[3].classes[0] -2\nset scenario.governments[5].ally_classes[0] -2\narg govt_a 3\narg govt_b 5\nend\n' \
  | build/release/tools/microexec_evnova/host/evnova_microexec_host
```

## Known gaps

`g_player_max_armor_cache` is marked `unmapped`: `Ship_ComputeShipMaxArmor`
(0x004637a0) reads the player's lazily cached max armor, while the port's
`NovaAi_ComputeMaxArmorPoints` recomputes it from the class and outfits. They
agree in a real game, but the invented cache is independent of the invented
class, so those runs are `not-comparable` instead of falsely mismatching.

## Tests

```sh
tools/.venv/bin/python -m unittest discover -s tools/microexec_evnova
```
