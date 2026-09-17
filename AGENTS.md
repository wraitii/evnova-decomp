# Instructions

Escape Velocity Nova is Ambrosia Software's 2002 open-world 2D space-trading and combat RPG. Its SDL3-based C++ reimplementation under `src/` is well advanced. Work improves both implementation coverage and Ghidra's disassembly, decompilation, types, and symbols to support further reconstruction.

## Workflow notes

- Before diving into an unfamiliar subsystem (data formats, resource loading,
  UI/window layout, render paths), run `ls docs/` to see if a reverse-engineering
  note already exists for it (e.g. `dlog_ditl_dialog_format.md` for dialog
  resources, `scenario_data_loading.md`, `menu_graphics_structure.md`). Docs are
  the fastest ground-truth shortcut over re-deriving from the Ghidra API.
- Always improve ghidra when possible.
- Check the EVN bible ("EV Nova Bible.html") for information.
- ResForge, an extensive EVN resource editor is bundled at assets/ResForge. Check the source for .rez reference.
- The pilot format is documented at docs/reference/pilotformat.txt
- Reference decompiled / disassembled ground truth from the Ghidra API.
- Be conservative with speculative renames. Rename only when behavior is clearly supported by decompile + callsites; otherwise keep neutral names and mark "Provisional" in comments.
- In ghidra: Prefer plate comments for functions; pre-comments for globals/data.
- When renaming high-level control-flow functions (startup, run loop, shutdown), also add a short clean-room comment block (2-4 lines) documenting purpose, entry/exit conditions, and confidence/unknowns.
- Prefer reproducible tests where practical; some gameplay situations are difficult to reproduce. Ask the user for gameplay validation when needed, and always ask before using the probe.
- **External probe harness**: `EVN_PROBE=1` starts a localhost HTTP control surface (pause/step, input injection, state reads, screenshots, log tailing). See `docs/probe_harness.md`. Any new game loop must present through `SdlPlatform::Present()` (not `SDL_RenderPresent`) and poll input through the existing platform channels so the probe keeps working everywhere.

## Function progress trackers (`decomp-progress.tsv` + `decomp-skipped.tsv`)

These root-level TSVs must be disjoint and jointly cover the decompile dump with **one row per Ghidra function** (`tools/ref_audit.py`).

`decomp-progress.tsv` is the canonical reimplementation tracker: `address\tname\timpl_file\treimpl_pct\tcomment`.

- **Update affected rows in the same change** as the implementation. `impl_file` is the `src/...` path (empty at 0%); `comment` records confidence, remaining gaps, and deliberate divergences.
- Estimate completeness conservatively: **0%** unported, regardless of Ghidra annotations; **10–30%** skeleton/cadence only; **40–90%** substantial but incomplete; **100%** faithful reimplementation.
- **Never dump or rewrite the whole file.** Locate target addresses with `rg` and patch only affected rows in place.

`decomp-skipped.tsv` records functions deliberately not ported: SDL3/OS/codec replacements and game-code functions with no port need: `address\tname\tlibrary\tcomment`.

- `library`: `blitter | qtml-iml | msl-crt | crt | winsock | vorbis | libpng | libjpeg | redundant`. Mirror Ghidra library renames into `name`. Use `redundant` for game-code functions the port has no need for: pure compile-time constant setters/seeds, dead or no-op routines, and behavior the port expresses structurally elsewhere rather than as a runtime call. Record the rationale in `comment`.
- Move rows from progress only with a deliberate decision recorded in `comment`. Keep the glue strip (~0x004E7389–0x00514593) and late-linked game code (≥0x00569C9C) in progress until triaged.

## C++ reimplementation phase

The purpose of this reimplementation is to have identical gameplay to the original, but the rendering, audio & such will be swapped out for SDL3.

- Preserve original game behavior, constants, and quirks first; improve architecture second. Do not “clean up” strange original behavior.
- Mark intentional corrections to confirmed bugs in the original executable or shipped scenario data as `BUGFIX(original)`, gate them through the shared compatibility policy, and always flag them to the user before implementing them. Do not use this marker for ordinary SDL/platform divergences.
- Trace every reimplemented Ghidra function to its original binary address; keep its progress row current.
- Citation format: `// Ghidra 0xaabbccdd Original_Name.` directly above the port function. When one original function is spread across port helpers, cite it at the primary site and name the others in the same comment ("… runs inline in X"), not one citation per fragment.
- Divergence/skip markers: plain `TODO(decomp)` for an unported scope; `TODO(decomp(0xaabbccdd)) skipped: <reason>` when original behavior is known and deliberately not reproduced. Never leave a comment-only `if` block as a deferral marker.
- Citation and TODO markers may live in the sibling header when the port is a header inline; the audit (`tools/ref_audit.py`) checks both.
- Check the Ghidra decompilation before writing or modifying a port; consult disassembly when needed. Document previously undescribed behavior in Ghidra before implementing it.
- Log unported behavior, divergences, and skips with `NovaLog::Todo()` (the project's TODO logger). In very hot paths, a TODO comment may replace runtime logging. Record remaining gaps in `decomp-progress.tsv` either way.
- Use straightforward C++23: small functions, early returns, `enum class`, `std::span`, `std::optional`, and strong domain types; avoid clever abstractions and template-heavy code.
- Prefer value types, RAII, and `std::unique_ptr` for deterministic ownership. No raw new/delete; raw pointers are non-owning. Wrap SDL handles in small RAII types.
- Represent game state explicitly; avoid hidden globals and singletons, even if ownership differs from the original. Keep game logic independent of SDL where practical.
- Name concepts by purpose. Keep comments concise and limited to uncertainty with evidence, divergences, meaningful constants, and surprising flow. No journalling.
- Put small cross-cutting helpers in `src/util/` (namespace `evnova::util`) instead of copying them per translation unit: `byte_reader.hpp` (endian payload reads), `geometry.hpp` (rect hit-tests/offsetting, half-open vs inclusive), `math.hpp` (polar add, truncation), `format.hpp` (thousands grouping), `color.hpp` (`NovaRgbColor` -> `SDL_Color`). Game-wide helpers that need game types live as focused headers under `src/game/` (`nova_random.hpp`, `nova_math.hpp`, `pict_texture.hpp`, `button_label.hpp`). Add a focused header when the same helper shows up in more than one file, and `using` the names locally rather than re-declaring them.
- Log failures with enough context to reproduce them.
- Prefer tests for complex behavior with known expected results, such as data loading. Avoid tests that encode speculation or depend on excessive gameplay mocks; temporary exploratory tests need not be committed.
- Keep commits small and distinguish faithful reconstruction from deliberate workarounds.

## Validation

- Default iteration: `cmake --build build/release`; run relevant tests with `ctest --test-dir build/release --output-on-failure -R '<pattern>'` (omit `-R` for the full suite).
- For C++ changes, also build debug with `cmake --build build/debug`. Configure missing build directories with `cmake --preset release` / `cmake --preset debug`; these require `VCPKG_ROOT`.
- Treat compiler warnings as errors. Format changed C++ files with `clang-format -i`. Keep whole-build clang-tidy disabled; run it on affected translation units with `SDKROOT="$(xcrun --show-sdk-path)" /opt/homebrew/opt/llvm/bin/clang-tidy -p build/release src/path.cpp` (the check set and warnings-as-errors live in the repo-root `.clang-tidy`, which clang-tidy discovers automatically). On macOS, Homebrew LLVM's `clang-tidy` is not on `PATH` in the agent shell; use that absolute path and prefix it with the SDK root (`SDKROOT="$(xcrun --show-sdk-path)"`). Homebrew LLVM does not inherit AppleClang's implicit sysroot, so otherwise SDK headers such as `AvailabilityMacros.h` are not found.
- After reimplementation or tracker changes, run `python3 tools/ref_audit.py` with Ghidra available and inspect `analysis/ref_audit.txt`. Dump coverage is checked only when `/tmp/ghidra_full_decompile` exists.
- Documentation-only changes do not require builds or tests.

# Ghidra server API

Ghidra runs a server at `http://127.0.0.1:8166` (from the `ghidraLlm` extension) that provides disassembly/decompilation plus type/class data. Data may be imperfect; decompiled output is still the basis of reimplementation.

The detailed API reference is `docs/ghidra_api.md`; upstream documentation is at `../ghidraLlm/API_USAGE.md`. If endpoint behavior differs, trust `../ghidraLlm/src/main/java/com/ghidra/llm/core/BusinessLogic.java`. Project authorization rules below take precedence over approval prompts in API documentation or responses.

A simple wrapper lives at `tools/ghidra_api`, called like so:
`./tools/ghidra_api "functions?name_re=Init&limit=10"`
`./tools/ghidra_api "function/rename" '{"addr":"0x00412345","new_name":"some_name"}'`

Prefer the tool, but curl can otherwise be used directly. All endpoints return plain text.

## Common read endpoints

- `GET /program/info` — arch/endian/image_base/compiler_spec
- `GET /symbols?name=...|name_re=...&limit=...&start=...&end=...` — list symbols; requires `name` OR `name_re`
- `GET /functions?name=...|name_re=...&limit=...&start=...&end=...` — list functions; `name` optional (lists all); range filters entrypoints; regex uses `%7C` not `|`
- `GET /types?name=...|name_re=...&limit=...` — list types; requires `name` OR `name_re`
- `GET /function/{addr}` or `.../info` — metadata (name, signature, plate comment)
- `GET /function/{addr}/decompile` — decompile function
- `GET /function/{addr}/callers` — list calling functions
- `GET /function/{addr}/callees` — list called functions
- `GET /function/{addr}/xrefs` — xrefs to the function
- `GET /function/{addr}/strings` — strings referenced in the function body
- `POST /function/decompile` — `{addrs: [...]}` — batch decompile
- `POST /function/synthetic-decompile` — read-only CFG region view: `{entry|start, stops|stop, name?, inputs?, names?, output?, output_type?, exits?, max_blocks?}`. Stops are excluded boundaries; omit for a region ending at the parent's return. `names:off|auto|full` controls naming effort (default `auto`, five seconds; `full`, 60 seconds). Multiple exits return an exit code unless `exits:"off"`. `force_infer` is a deprecated alias for `names:"full"`.
- `POST /function/parent-view` — read-only overview of a parent with regions collapsed into calls to their synthetic views, plus each view decompiled: `{function|addr, regions:[{entry, stops?, name?, ...}], names?, bodies?, plan?, timeout?, max_blocks?}`; `plan:true` reports the recovered signatures and any `reentered` blocks without decompiling. A region with no `stops` runs to the parent's own return and collapses into a tail call — on Ship_HandlePlayerShipCore that leaves a 167-line skeleton (0.3s) instead of 3201 lines (49s)
- `POST /function/synthetic-suggestions` — propose a plan for breaking a function up: a set of non-overlapping single-entry regions (from real post-dominator trees) ready to paste into `/function/parent-view` or `/function/synthetic-decompile`, each scored via the same dataflow pass that recovers a view's signature (`EDI(ESI) +1 scratch` — takes `ESI`, returns `EDI`, plus live-outs): `{function|addr, max_regions?, min_score?, max_exits?, alternatives?, named_labels_as_starts?, min_blocks?, min_bytes?, max_blocks?, max_depth?}`; `min_score` (default `0.35`) is the real coverage control, `max_exits` (default `3`) allows multi-stop regions, `alternatives:false` for the plan alone, and early-return regions appear with a `[return]` boundary
- `GET /symbol/{addr}` — auto-dispatch to `/function/` or `/data/` depending on what's at the address
- `GET /labels/{addr}` — list every symbol at an address, including namespace, source, and primary/dynamic state
- `GET /data/{addr}` — label/type/size & xrefs for an address. Good to explore vtables.
- `GET /type/{name}/layout` (alias `info`) — size + members
- `GET /type/{name}/methods?start=...&end=...` — functions in class namespace
- `GET /type/{name}/xrefs` — cross-references to class methods
- `GET /type/{name}/uses` — global symbols of a type (useful for vtables)
- `GET /range/{start}/{end}/disasm` — disassembly for address range (code or data). Good to explore vtables.
- `GET /range/{addr}/disasm` — disassemble the basic block containing an address
- `GET /range/{start}/{end}/bytes` — hexdump of bytes in range
- `GET /function/{addr}/cfg` or `/range/{start}/{end}/cfg` — compact basic-block CFG with flow-labelled destinations
- `GET /operand_search?op={scalar}&filter=...&context_filter=...&before=...&after=...&start=...&end=...` — find functions containing instructions using a scalar operand (memory displacements, immediates); `op` is decimal or 0x-hex. Use `/operand_search/decomp` or `/operand_search/disasm` for context views at the site.

## Write endpoints

All Ghidra changes needed for the task are preauthorized, including `confirm:true`, struct resizing/field replacement, signature resets, and label changes. Inspect the target and supporting evidence first; no additional user approval is needed, even if an API response asks for it.

- `POST /symbol/rename` — `{addr, new_name}`
- `POST /function/rename` — `{addr, new_name}` — no `::` allowed; see reclassify for class methods.
- `POST /function/reclassify` — `{addr, class, new_name?}` — class must match existing struct/comp type; creates class namespace if missing
- `POST /class/create` — `{class_name, struct_size?}` — creates a class/namespace for reclassify
- `POST /type/rename` — `{name, new_name}` — rename a type
- `POST /struct/modify_field` — `{struct, offset, name, data_type, comment?}` — modify a struct field at a specific offset
- `POST /type/tag_vtable` — `{type, vtable_type, vtable_addr, set_field_type_if_undefined?}` — standardize a class's vtable link
- `POST /symbol/retype` — `{addr, data_type}` — retype a global symbol (supports pointers, arrays)
- `POST /comment/set` — `{addr|function_addr, comment, kind=plate|pre|post|eol}`
- `POST /function/signature` — `{addr, return_type?, name?, a0?, a1?, ...}` (param keys use actual names from decomp output, e.g., `a0`, `player`, `flags`; only explicitly supplied fields are committed)
- `POST /function/signature/reset` — `{addr, confirm: true}` — remove the committed prototype and restore type inference while preserving name, namespace, comments, locals, and calling convention
- `POST /label/create` — `{addr, name, primary?, confirm?}` — create a label in the containing function namespace, or globally when outside a function
- `POST /label/delete` or `/label/set_primary` — `{addr, name, confirm?}` — operate only on label symbols, never function symbols
- `POST /full_decompile` — `{folder: "/abs/path"}` — decompile every function to one `.c` file per function

## Notes

- Regex params are `*_re`; invalid regex → 400. Regex requires URL escaping (e.g., `%7C` for `|`).
- `start`/`end` ranges apply to symbols/functions/type methods/xrefs/uses; both required when either is given.
- The `/operand_search` endpoint supports both disassembly and decompiled context views with `before`/`after` parameters.
- Struct resizing and field conflicts may require `confirm:true`; its use is preauthorized.
