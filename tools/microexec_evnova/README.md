# EV Nova microexec adapter

Connects [microexec](../../../microexec/README.md) to the original EV Nova
executable and its C++ port. From this repository, microexec discovers
`microexec.toml` automatically. Paths resolve from the repository root.

- `scenario.py` supplies resource bytes, loader context, and measured field
  distributions from shipped `Nova Data *.rez` archives.
- `scenario_providers.py` installs verified stock government record providers.
- `rules.json` maps original inputs and writes to port state.
- `host/` runs port functions with generated field and function reflection.

## Setup and comparison

```sh
python3 -m venv tools/.venv
tools/.venv/bin/pip install -r ../microexec/requirements.txt
cmake -S . -B build/release -DEVNOVA_MICROEXEC=ON
cmake --build build/release --target evnova_microexec_host

tools/.venv/bin/python ../microexec/microexec.py init 0x0046bc90
tools/.venv/bin/python ../microexec/microexec.py run 0x0046bc90 -n 20
tools/.venv/bin/python ../microexec/microexec.py diff 0x0046bc90
# Request LLM cases for uncovered edges explicitly:
tools/.venv/bin/python ../microexec/microexec.py coverage 0x0046bc90
```

Ghidra must be available at the profile's URL. The engine reads credentials
from `../microexec/.env`; see its README for model setup and search commands.
Callees execute by default, with RNG supplied by the scripted stream. Older
automatic stubs migrate to execution and their suite cases replay. Recorded
assumption readers keep unrelated callee-only inputs out of parent-edge search.
Explicit stub experiments still support the reconciliation and repair commands below.

`run` uses Jev priors and construction feedback by default; add
`--no-model` for a model-free run. LLM coverage proposals run
only through the explicit `coverage` command.
Outside this repository, pass `--project /path/to/tools/microexec_evnova/microexec.toml`
before the command.

`diff` replays suite runs with callees executed before translating initial
inputs to the host. Inspect `status` for mapping and construction gaps:
`read_unset_mapped` means the port read original state the case did not seed;
`read_unset_unmapped` means a port field needs a bridge rule. A return match
with either gap is incomplete evidence.

Changed stub witnesses can be repaired at parent scope using existing callee
cases, followed by a model fallback for unresolved cases:

```sh
tools/.venv/bin/python ../microexec/microexec.py repair Ship_DoesShipLikePlayer --no-model
tools/.venv/bin/python ../microexec/microexec.py repair Ship_DoesShipLikePlayer --attempts 4 --model-attempts 2
```

Successful repairs preserve the original parent return and branch edges and
validate the sampled return goals with callees executed. They create new cases;
original witnesses and rejected hypotheses remain in the database history.
The hypotheses are case-local, so they do not narrow stock or synthetic generators.

## Stock scenario providers

```sh
# Validate and inject stock scenario records as generator options:
tools/.venv/bin/python tools/microexec_evnova/scenario_providers.py --check
tools/.venv/bin/python tools/microexec_evnova/scenario_providers.py
tools/.venv/bin/python tools/microexec_evnova/scenario_providers.py --remove
# Optional isolated world ledger, or one family at a time:
tools/.venv/bin/python tools/microexec_evnova/scenario_providers.py --state-dir /tmp/evnova-microexec
tools/.venv/bin/python tools/microexec_evnova/scenario_providers.py --check --family weapons
```

The installer validates every record against its Ghidra struct and adds 2426
observed partial record providers:

| family | global | shipped records |
|--------|--------|-----------------|
| governments | `g_government_defs` | 68 |
| ships | `g_ship_class_defs` | 288 |
| outfits | `g_outfit_defs` | 242 |
| weapons | `g_weapon_defs` | 81 |
| stellars | `g_stellar_defs` | 411 |
| systems | `g_system_defs_ptr` | 545 |
| fleets | `g_random_encounter_fleet_defs` | 128 |
| dudes | `g_dude_defs` | 147 |
| personalities | `g_pers_defs` | 516 |

Each provider preserves its actual table index. Governments carry the hand-verified
flags/relation window (class numbers copied unchanged; `-1` marks unused relations).
Every other family carries the **clean direct payload copies** of its port decoder:
a Ghidra scalar whose only appearance in `src/game/scenario_data.cpp` is one
literal-offset `ReadBeNN(bytes, …)` assignment, at least as wide as the payload
read, and whose name resolves through the bridge aliases. Derived, rebased,
clamped, float and string fields are deliberately omitted, so each partial record
is a factual payload copy and its omitted fields fall through to the normal
generators. A narrower payload into a wider integer is the loader's sign-extension
(e.g. `short base_shield` → `int base_shield_points`). Reinstallation preserves
unchanged revisions and unrelated world definitions.

Provider definitions live here in `scenario_providers.py`, and values come from
the shipped archives. Rerun the injection script after deleting `analysis/`.
Learned weights, generated cases, and reports still live under `analysis/`.

Scenario records are named **alternative parent samplers**, with recorded provider
identity and revision. They coexist with LLM samplers, including samplers with the
same scope. Their `observed` evidence describes the record's factual provenance;
it does not force selection over synthetic generators. Each source gets a Jev
`provider_choice` weight, and fuzzing can choose either source. Omitted fields
fall through to other generators. Explicit human pins, case assignments, requested
providers, and construction constraints retain precedence. Installation updates
copied target ledgers too; old runs keep their data and may become superseded when
the provider revision changes.

The evidence plugin stays read-only. Providers cover stock archives, not
installed plug-ins or dynamic caches; omitted fields fall through to normal
construction. Missions (the original `MisnDef` table) still supply evidence only.

Providers propose data without cross-field `read()`. Relationships belong in
initial-state construction constraints. Saved cases preserve concrete inputs
and provenance across definition changes; mutations reconstruct dependent
inputs. Constraints never repair program writes. See the engine README for
constraint syntax and fixed assignments.

## Host and bridge

Reflection exposes scalar members, arrays, vectors, bitsets, and supported
`@port`-cited functions. Run `echo describe | build/release/tools/microexec_evnova/host/evnova_microexec_host`
to inspect fields, targets, and unsupported signatures. Rebuild after changing
port declarations or citations.

Bridge rules describe data rather than binary layout. Automatic matching
handles same-named fields and numbered array fields; explicit rules cover
renames, skips, rebased IDs, and reordered arguments. In particular,
`GovtDef.rank_privileges[0/1]` maps to the port's named
`rank_privileges.no_auto_attack` / `rank_privileges.always_land` fields.
Explicit array-element rules take precedence over wildcard rules.
`Outfit_ComputeScaledPurchasePrice` takes `(price, scale, item_tech, stellar_tech)`
while the port takes `(price, item_tech, stellar_tech, scale)`.

String inputs travel as hex bytes through the first recorded NUL. Missing or
conflicting bytes, null pointers, missing terminators, input writes, or strings
above 65,535 bytes make a run `not-comparable`. `CString_Length` executes on
actual bytes; string members of records and string returns remain unsupported.

Standalone `NovaExpression_EvaluateBoolean` (`0x00449020`) runs model its
external entry: the sole nonrecursive caller, `Mission_CheckReactionConditionSatisfied`
(`0x00447f20`), bypasses blank conditions and initializes the cursor to zero.
Register these requirements again when using a fresh state directory:

```sh
tools/.venv/bin/python ../microexec/microexec.py constraint 'arg.param_1[*]' requirement --target 0x00449020 --id 'contract:expression:entry-cursor' --expr 'value == 0' --mandatory --text '0x00447f20 initializes the external-entry cursor to zero.'
tools/.venv/bin/python ../microexec/microexec.py constraint 'arg.param_1[*]' requirement --target 0x00449020 --id 'contract:expression:nonempty-buffer' --expr 'read("g_reaction_script_buffer[0]") != 0' --mandatory --text '0x00447f20 returns true for blank conditions without calling the evaluator.'
```

These requirements constrain initial state only; recursive entries, including
empty groups, remain exercised during execution. Saved cases preserve their old
inputs, so earlier comparisons can still show witnesses outside this contract.

The default instrumented host reports writes and reads of unset scalar state.
Library bulk reads, wide struct copies, and newly appended vector elements
have tracing limits. `EVNOVA_MICROEXEC_TRACE=OFF` supplies snapshot writes only.
Struct returns/out-parameters and independently sampled derived caches remain
comparison gaps, including `Mission_ComputeDateAfterSteps`. See source comments
and bridge-rule reasons for target details.

Player max-armor comparisons use a cold-cache policy. Before comparing a target
that calls `Ship_ComputeShipMaxArmor`, register the shared generator:

```sh
tools/.venv/bin/python ../microexec/microexec.py sampler global_sampler g_player_max_armor_cache --expr '-1.0' --evidence review --text 'Cold-cache comparison: force original class/outfit recomputation; omit warm-cache reuse and the license-token override.'
```

The bridge skips this internal cache and its fill writes. The original recomputes
for negative cache inputs, as the port does; warm-cache reuse and the license-token
override are outside this policy. Recorded or explicit populated-cache inputs
must not be compared under this skip. The generator lives in the local ledger,
so register it again when using a fresh state directory.

## Tests

```sh
# Adapter, resource decoding, prompts, and native string protocol:
tools/.venv/bin/python -m unittest discover -s tools/microexec_evnova
# Original executable + native host; Ghidra and stock archives required:
EVN_MICROEXEC_LIVE=1 tools/.venv/bin/python -m unittest discover -s tools/microexec_evnova -p test_live.py -v
```

Live tests use temporary state and disable model calls. They cover government
relations and boundaries, local overrides, persisted replay, mutation and
constraint diagnostics, and floating point price boundaries. They reject
matches with unseeded port reads. They do not use the game probe.

On this macOS setup, Unicorn 2.1.4 traps in `init_cache_info` during `mem_map`
inside the Codex sandbox; the live tests work outside the sandbox.
