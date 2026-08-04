# Instructions

Escape Velocity Nova is Ambrosia Software's 2002 open-world 2D space-trading and combat RPG, with nonlinear faction storylines, ship progression, and a dynamic galaxy economy.

## Current state: recompilation started.

The decomp is now quite advanced.
The current goal is to continue improving decomp understanding and metadata — function/param/type renames, struct fields, globals, and comments in the Ghidra DB. There is no C++ code in this repo yet; reimplementation has not started. Many *deep* helper routines and low-level subsystems are still only provisionally named, and numerous struct fields remain untyped/unattributed.

Ghidra is the decompiler backend — it maintains its own database of the code, with disassembled, decompiled, partial type and symbol information.
Part of the objective of this metadata work is to improve Ghidra's DB to make future decompiling easier.

The day-to-day workflow is function-centric metadata analysis: see `exploring.txt` for the recipe, and `tools/` for analysis scripts (`function_cluster_v1.py`, `function_cluster_v2.py`, `cleanup_all_funcs.py`) and the `ghidra_api` wrapper.

## Workflow notes

- Reference decompiled / disassembled ground truth from the Ghidra API.
- When grepping, prefer to do it in the repo root and `tools/`; data may not be where you expect.
- Be conservative with speculative renames; be liberal with factual comments. Rename only when behavior is clearly supported by decompile + callsites; otherwise keep neutral names and mark "Provisional" in comments.
- Prefer plate comments for functions; pre-comments for globals/data.
- When renaming high-level control-flow functions (startup, run loop, shutdown), also add a short clean-room comment block (2-4 lines) documenting purpose, entry/exit conditions, and confidence/unknowns.

## C++ reimplementation phase

The purpose of this reimplementation is to have identical gameplay to the original, but the rendering, audio & such will be swapped out for SDL3.

- Preserve original game behavior first; improve architecture second.
- Use modern C++23, but avoid clever abstractions and template-heavy code.
- Prefer value types, RAII, std::unique_ptr, and deterministic ownership.
- No raw new/delete. Raw pointers are non-owning only.
- Keep SDL handles behind small RAII wrappers.
- Keep game logic independent of SDL wherever practical.
- Represent game state explicitly. Avoid hidden globals and singleton managers. This may diverge from the original implementation where practical.
- Preserve original constants and quirks when they affect gameplay.
- Name reconstructed concepts by purpose, not by decompiler-generated names.
- Document uncertain behavior with TODO(decomp) and supporting evidence. Log verbosely.
- Prefer small functions, early returns, and straightforward control flow.
- Use enum class, std::span, std::optional, and strong domain types.
- Log failures with enough context to reproduce them.
- Treat compiler warnings as errors.
- Format automatically with clang-format; lint with clang-tidy.
- Don't overdo regression tests - good for complex, easily testable functions. Bad for gameplay in general as it's too hard to setup.
- Keep commits small and distinguish faithful reconstruction from deliberate fixes.
- Do not “clean up” strange original behavior.

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
