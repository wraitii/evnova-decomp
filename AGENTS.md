# Instructions

Escape Velocity Nova is Ambrosia Software's 2002 open-world 2D space-trading and combat RPG, with nonlinear faction storylines, ship progression, and a dynamic galaxy economy.

## Current state: recompilation is well advanced; a C++ reimplementation exists under `src/` (SDL3-based). Ongoing work improves both decomp understanding/metadata and reimplementation coverage.

Ghidra is the decompiler backend — it maintains its own database of the code, with disassembled, decompiled, partial type and symbol information.
Part of the objective of this metadata work is to improve Ghidra's DB to make future decompiling easier.

The day-to-day workflow is function-centric metadata analysis: see `exploring.txt` for the recipe, and `tools/` for analysis scripts (`function_cluster_v1.py`, `function_cluster_v2.py`, `cleanup_all_funcs.py`) and the `ghidra_api` wrapper.

## Workflow notes

- Before diving into an unfamiliar subsystem (data formats, resource loading,
  UI/window layout, render paths), run `ls docs/` to see if a reverse-engineering
  note already exists for it (e.g. `dlog_ditl_dialog_format.md` for dialog
  resources, `scenario_data_loading.md`, `menu_graphics_structure.md`). Docs are
  the fastest ground-truth shortcut over re-deriving from the Ghidra API.
- Check the EVN bible ("EV Nova Bible.html") for information.
- Reference decompiled / disassembled ground truth from the Ghidra API.
- When grepping, prefer to do it in the repo root and `tools/`; data may not be where you expect.
- Be conservative with speculative renames. Rename only when behavior is clearly supported by decompile + callsites; otherwise keep neutral names and mark "Provisional" in comments.
- In ghidra: Prefer plate comments for functions; pre-comments for globals/data.
- When renaming high-level control-flow functions (startup, run loop, shutdown), also add a short clean-room comment block (2-4 lines) documenting purpose, entry/exit conditions, and confidence/unknowns.
- If testing game behaviour is required, stop and ask the user for input - you will not be able to interact with the game well enough.
- Beware of `find` in shell, on macos some commands are very slow if searching the disk.
- **DO NOT use tail/head with cmake build**, or add a small timeout. tail/head can hang if there are fewer lines output than expected.

## Function progress tracker (`progress.csv`)

The project root `progress.csv` tracks one row per Ghidra function:
`address,name,impl_file,reimpl_pct,comment`. It is the canonical record of how
much of each function has been re-implemented.

- **Keep it always up to date**, conservatively, whenever you make a code change that re-implements or partially re-implements a Ghidra function: update that row's `reimpl_pct`, `impl_file`, and `comment` in the same commit.
- `reimpl_pct` is an **estimate of reimplementation completeness**, 0% to 100%.
  - `100%` — faithful reimplementation.
  - `10%`–`90%` — partial: the higher the number, the more behavior is reimplemented (skeleton/cadence-only ≈ 10–30%; a substantial subset that still has known stubbed scopes or untracked fields ≈ 40–90%).
  - `0%` — not reimplemented (regardless of whether the function is already named/annotated in Ghidra; only reimplementation progress counts here).
- `impl_file` is the `src/...` path that reimplements the function (empty when `0%`).
- `comment` is a short note (confidence, known gaps/divergences, TODO(decomp)).
- **Edit `progress.csv` in place**.
- **`progress.csv` is very large (~3200 rows). Never rewrite it wholesale or dump it to your context. Always locate the target address with grep and make surgical, in-place edits (edit tool / patch), leaving all other rows intact.**

## C++ reimplementation phase

The purpose of this reimplementation is to have identical gameplay to the original, but the rendering, audio & such will be swapped out for SDL3.

- Preserve original game behavior first; improve architecture second.
- Keep a one to one correspondance between reimplementation and ghidra functions. A function re-implementing ghidra functionality should have a comment to the address in the original binary.
- Citation format: `// Ghidra 0xaabbccdd Original_Name.` directly above the port function. When one original function is spread across port helpers, cite it at the primary site and name the others in the same comment ("… runs inline in X"), not one citation per fragment.
- Divergence/skip markers: plain `TODO(decomp)` for an unported scope; `TODO(decomp(0xaabbccdd)) skipped: <reason>` when original behavior is known and deliberately not reproduced. Never leave a comment-only `if` block as a deferral marker — either port the call or log the skip.
- Citation and TODO markers may live in the sibling header when the port is a header inline; the audit (`tools/ref_audit.py`) checks both.
- If running in undescribed code in ghidra, document it in ghidra first. Feel free to stop coding and do an explanatory improvement pass on ghidra in these cases.
- Log all divergences or skips when writing new code.
- When writing or modifying a ghidra-decomp-available function, always check it's ghira decompilation. If necessary, the disassembly can also be checked.
- Use modern C++23, but avoid clever abstractions and template-heavy code.
- Prefer value types, RAII, std::unique_ptr, and deterministic ownership.
- No raw new/delete. Raw pointers are non-owning only.
- Keep SDL handles behind small RAII wrappers.
- Try to keep game logic independent of SDL, though it's sometimes more practical to bundle the two together.
- Represent game state explicitly. Avoid hidden globals and singleton managers. This may diverge from the original implementation where practical.
- Keep comments minimal, the code should be self-explanatory.
- NO JOURNALLING in the comments.
- Preserve original constants and quirks when they affect gameplay.
- Name reconstructed concepts by purpose, not by decompiler-generated names.
- Document uncertain behavior with TODO(decomp) and supporting evidence. Log verbosely.
- Avoid over-commenting. Only comment explicit divergences, assumptions, or particularly complicated or surprising code flow. The rest should be self-explanatory, mostly. You may comment constants & other game-provided data especially where hardcoded numbers have meaning.
- Prefer small functions, early returns, and straightforward control flow.
- Use enum class, std::span, std::optional, and strong domain types.
- Log failures with enough context to reproduce them.
- Treat compiler warnings as errors.
- Format automatically with clang-format; lint with clang-tidy.
- Avoid tests for provisional code. Only test complex algorithms (such as data loading) where the result is known final and accurate, and implementation difficult. You may write temporary tests when building that we drop on committing (once they pass, they pass). Avoid testing simple gameplay behaviour where the test is too mocked to be relevant.
- Keep commits small and distinguish faithful reconstruction from deliberate workarounds.
- Do not “clean up” strange original behavior.
- Build both debug and release builds.

# Ghidra server API

Ghidra runs a server at `http://127.0.0.1:8166` (from the `ghidraLlm` extension) that provides disassembly/decompilation plus type/class data. Data may be imperfect; decompiled output is still the basis of reimplementation.

The full, authoritative API reference is `docs/ghidra_api.md`, kept in sync with the server source at `../ghidraLlm/API_USAGE.md`. The summary below is checked against `BusinessLogic.java`; if the two disagree, trust the code.

A simple wrapper lives at `tools/ghidra_api`, called like so:
`./tools/ghidra_api "functions?name=Init|init&limit=10"`
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
- `GET /symbol/{addr}` — auto-dispatch to `/function/` or `/data/` depending on what's at the address
- `GET /data/{addr}` — label/type/size & xrefs for an address. Good to explore vtables.
- `GET /type/{name}/layout` (alias `info`) — size + members
- `GET /type/{name}/methods?start=...&end=...` — functions in class namespace
- `GET /type/{name}/xrefs` — cross-references to class methods
- `GET /type/{name}/uses` — global symbols of a type (useful for vtables)
- `GET /range/{start}/{end}/disasm` — disassembly for address range (code or data). Good to explore vtables.
- `GET /range/{start}/{end}/bytes` — hexdump of bytes in range
- `GET /operand_search?op={scalar}&filter=...&context_filter=...&before=...&after=...&start=...&end=...` — find functions containing instructions using a scalar operand (memory displacements, immediates); `op` is decimal or 0x-hex. Use `/operand_search/decomp` or `/operand_search/disasm` for context views at the site.

## Write endpoints (ask before using)

- `POST /symbol/rename` — `{addr, new_name}`
- `POST /function/rename` — `{addr, new_name}` — no `::` allowed; see reclassify for class methods.
- `POST /function/reclassify` — `{addr, class, new_name?}` — class must match existing struct/comp type; creates class namespace if missing
- `POST /class/create` — `{class_name, struct_size?}` — creates a class/namespace for reclassify
- `POST /struct/modify_field` — `{struct, offset, name, data_type, comment?}` — modify a struct field at a specific offset
- `POST /type/tag_vtable` — `{type, vtable_type, vtable_addr, set_field_type_if_undefined?}` — standardize a class's vtable link
- `POST /symbol/retype` — `{addr, data_type}` — retype a global symbol (supports pointers, arrays)
- `POST /comment/set` — `{addr|function_addr, comment, kind=plate|pre|post|eol}`
- `POST /function/signature` — `{addr, return_type?, name?, a0?, a1?, ...}` (param keys use actual names from decomp output, e.g., `a0`, `player`, `flags`)
- `POST /full_decompile` — `{folder: "/abs/path"}` — decompile every function to one `.c` file per function

## Notes

- Regex params are `*_re`; invalid regex → 400. Regex requires URL escaping (e.g., `%7C` for `|`).
- `start`/`end` ranges apply to symbols/functions/type methods/xrefs/uses; both required when either is given.
- The `/operand_search` endpoint supports both disassembly and decompiled context views with `before`/`after` parameters.
- Struct field modification requires explicit confirmation for dangerous operations (struct resizing, field conflicts).
