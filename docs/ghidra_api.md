## Overview

The Ghidra API provides access to disassembly, decompilation, and type/class data from the Ghidra database. This is essential for the reimplementation process, allowing us to reference ground truth from the original binary.

API runs locally at `http://127.0.0.1:8166`.

## Basic Endpoints

### GET /program/info

Basic arch/endian/image_base/compiler_spec endpoint. Useful to check if the API is up and verify the program is loaded correctly.

## Searching

### GET /symbols

Query symbols (globals, functions, types) from the decomp. One of name/name_re is required.

**Parameters:**

- `name` - partial name matching (case-sensitive, returns symbols containing the string)
- `name_re` - name filtering using regexes (requires URL escaping, case-insensitive).
- `limit?` - limit number of results
- `start?`/`end?` - filter on address range

**Examples:**

```bash
# Find symbols matching the pattern 'Game' or 'Init'. Regexes are case-insensitive.
curl -s "http://127.0.0.1:8166/symbols?name_re=Game%7CInit&limit=20"

# Find symbols in address range
curl -s "http://127.0.0.1:8166/symbols?start=0x401000&end=0x402000"
```

### GET /functions

Query function symbols. Like `/symbols`, for functions/methods only.

**Parameters:**

Same as /symbols

**Examples:**

```bash
# Find a specific function
curl -s "http://127.0.0.1:8166/functions?name=MapRegion::Constructor"

# Find all methods in a class
curl -s "http://127.0.0.1:8166/functions?name=MapRegion::"
```

### GET /types

Query type definitions (structs, classes, unions). Like `/symbols`, for types.

**Parameters:**

Same as /symbols

## Function Analysis

### GET /function/{addr}

Get basic information about a function (address, name, signature).

**Example:**

```bash
curl -s "http://127.0.0.1:8166/function/0x401234"
```

**Note:** This is equivalent to `/function/{addr}/info`

### GET /function/{addr}/info

Get metadata about a function (size, signature, etc).

**Note:** `/function/{addr}` (without action) now defaults to `/function/{addr}/info` for convenience.

### GET /function/{addr}/decompile

Get decompiled C code for a function. Prefer the batch endpoint when decompiling several functions.

**Example:**

```bash
curl -s "http://127.0.0.1:8166/function/0x401234/decompile"
```

### GET /function/{addr}/callers

List all functions that call the specified function.

**Example:**

```bash
curl -s "http://127.0.0.1:8166/function/0x401234/callers"
```

### GET /function/{addr}/callees

List all functions called by the specified function.

**Example:**

```bash
curl -s "http://127.0.0.1:8166/function/0x401234/callees"
```

### GET /function/{addr}/strings

Get strings referenced in the function body.

**Example:**

```bash
curl -s "http://127.0.0.1:8166/function/0x401234/strings"
```

**Note:** Prefer decompilation for string analysis - this endpoint misses localized strings.

### POST /function/decompile

Batch decompile multiple functions.

**Body:**

```json
{
  "addrs": ["0x401234", "0x401500", "0x401780"]
}
```

**Example:**

```bash
curl -s -X POST "http://127.0.0.1:8166/function/decompile" \
  -H "Content-Type: application/json" \
  -d '{"addrs": ["0x401234", "0x401500"]}'
```

## Data Analysis

### GET /symbol/{addr}

Unified endpoint that automatically dispatches to either `/function/` or `/data/` based on what's found at the address. Useful when you're unsure whether an address contains code or data.

**Examples:**

```bash
# Get info about an address (automatically detects function vs data)
curl -s "http://127.0.0.1:8166/symbol/0x401234"
```

### GET /data/{addr}

Get information about a data item at an address (label, type, size, xrefs). Returns an error if the address contains a function.

**Note:** Xrefs are limited to 20 by default for readability.
If the item looks like a vtable (type/symbol name contains `vt`/`vtable`), this endpoint returns a metadata header plus expanded table contents (same style as `/range/.../disasm`) instead of the short summary.

**Error Handling:** If you query a function address, the error message will suggest using `/function/{addr}` instead.

### GET /type/{name}/layout

Get struct/class layout including members and their offsets.

**Example:**

```bash
curl -s "http://127.0.0.1:8166/type/MapRegion/layout"
```

**Note:** Ghidra only shows fields that have been discovered/typed in the decomp. Some fields may be absent simply because Ghidra hasn't identified them yet.

### GET /type/{name}/methods

List all methods associated with a class/type.

**Example:**

```bash
curl -s "http://127.0.0.1:8166/type/MapRegion/methods"
```

### POST /type/tag_vtable

Standardize a class/struct's vtable link by:
- adding a machine-readable comment on field `0` with the vtable type/address tag
- optionally retyping a concrete vtable address to `vtable_type`

**Body:**

```json
{
  "type": "UiElemButton",
  "vtable_type": "UiElementVTable",
  "vtable_addr": "0x005dd870",
  "set_field_type_if_undefined": true
}
```

`set_field_type_if_undefined` is optional (default `false`). When enabled, field `0` is retyped to `vtable_type *` and named `vt` only if the current field `0` data type is undefined.

### GET /range/{start}/{end}/disasm

Get disassembly for an address range. Works with code and data.
Useful when analyzing vtables or actual data tables.

**Example:**

```bash
# Analyze vtable at known location
curl -s "http://127.0.0.1:8166/range/0x40a000/0x40a100/disasm"
```

### GET /range/{start}/{end}/bytes

Get hexdump of bytes in range.

## Advanced Search

### GET /operand_search

Find functions containing instructions that use a specific operand value (including memory displacements and immediates).
Very useful for finding virtual function calls. Useful for working out what struct fields means.
Do note that ghidra will simply show you all instructions, so for e.g. virtual calls the output may contain unrelated classes so long as they use the same offsets.
This basic version of the endpoint will list functions, which you can then decompile, or you can use the /disasm or /decomp variants.
**Parameters:**

- `op` - operand scalar value to search for (either decimal or 0x-prefixed hex.)
- `filter?` - regex to filter full instruction text (e.g., `call`, `push`, `lea|mov`). Defaults to `.*`
- `context_filter?` - regex to filter nearby disassembly context (case-insensitive). Uses `before/after` lines if provided, otherwise defaults to 5/5 for context filtering.
- `start?` / `end?` - optional inclusive function entrypoint address range to restrict the search to (hex address strings)

**Examples:**

```bash
# Find functions with virtual calls to vtable offset 0x10
curl -s "http://127.0.0.1:8166/operand_search?op=0x10&filter=call"

# Find lea/mov using operand 0x14 (for struct fields)
curl -s "http://127.0.0.1:8166/operand_search?op=0x14&filter=lea%7cmov"

# Find push 0x10 only when nearby context mentions vtable-ish loads/calls
curl -s "http://127.0.0.1:8166/operand_search?op=0x10&filter=push&context_filter=call|mov"

# Restrict to functions with entrypoints in a specific range
curl -s "http://127.0.0.1:8166/operand_search?op=0x10&start=0x401000&end=0x40ffff"
```

### GET /operand_search/decomp

Similar to `/operand_search` but returns decompilation context at the actual site.

**Parameters:**

- `op` - operand scalar value to search for (either decimal or 0x-prefixed hex.)
- `filter?` - regex to filter instructions (e.g., `call`). Defaults to `.*`
- `context_filter?` - regex to filter nearby disassembly context before decomp output selection
- `before?`/`after?` - how many lines of context to include.
- `start?` / `end?` - optional inclusive function entrypoint address range to restrict the search to

**Example:**

```bash
curl -s "http://127.0.0.1:8166/operand_search/decomp?op=0x10&filter=call&before=8&after=2"
```

### GET /operand_search/disasm

Similar to `/operand_search` but returns disassembly context at the actual site.
This is more reliable than /decomp but less useful. Good sanity check.

**Parameters:**

- `op` - operand scalar value to search for (either decimal or 0x-prefixed hex.)
- `filter?` - regex to filter instructions (e.g., `call`). Defaults to `.*`
- `context_filter?` - regex to filter nearby disassembly context (can match adjacent lines)
- `before?`/`after?` - how many lines of context to include.
- `start?` / `end?` - optional inclusive function entrypoint address range to restrict the search to

**Example:**

```bash
curl -s "http://127.0.0.1:8166/operand_search/disasm?op=0x10&before=8&after=2"
```

## Specialized Read Endpoints

These are generally avoided unless absolutely needed - prefer the main endpoints above.

### GET /function/{addr}/strings

Get strings referenced in the function body. **Note:** Prefer decompilation for this - this endpoint misses localized strings.

### GET /type/{name}/xrefs

Find all cross-references to class methods.

**Example:**

```bash
curl -s "http://127.0.0.1:8166/type/MapRegion/xrefs"
```

## Write Endpoints

⚠️ **WARNING:** These modify the Ghidra database. Use with caution and ask before using.

### POST /symbol/rename

Rename a symbol.

**Body:** `{addr: "0x401234", new_name: "g_GameEngine"}`

**Note:** Cannot be used to move functions to namespaces (names containing `::`). For class methods, use `/function/reclassify` instead.

### POST /function/rename

Rename a function. For class methods, use `/function/reclassify` instead.

**Body:** `{addr: "0x401234", new_name: "InitializeMap"}`

**Restriction:** Function names cannot contain `::` - use `/function/reclassify` to move functions to namespaces.

### POST /function/reclassify

Reclassify a function as a class method. Fails if the class isn't registered yet.

**Body:** `{addr: "0x401234", class: "MapRegion", new_name: "Constructor"}`

### POST /class/create

Create a new class/namespace. Always search for similar named classes first.

**Body:** `{class_name: "MapRegion", struct_size: 24}`

### POST /type/rename

Rename a type.

**Body:** `{name: "MapRegn", new_name: "MapRegion"}`

### POST /struct/modify_field

Modify a struct field at a specific offset.

**Body:** `{struct: "MapRegion", offset: 4, name: "id", data_type: "uint32_t", comment: "Region identifier"}`

**Note:** Requires explicit confirmation for dangerous operations (struct resizing, field conflicts).

### POST /symbol/retype

Retype a global symbol (data) at an address.

**Body:** `{addr: "0x401234", data_type: "MyStruct*"}`

Supports pointer types (`*`), arrays (`int[10]`), and case-insensitive type name matching.

### POST /comment/set

Add a comment to code or data.

**Body:** `{addr: "0x401234", comment: "Initializes map data", kind: "plate"}`

**Kinds:** `plate` (function header), `pre` (preferred), `post`, `eol` (end of line)

### POST /function/signature

Update function signature. All params are optional except addr, avoid setting un-necessary ones.

**Body:**

```json
{
  "addr": "00421f50", // function to operate on
  "return_type": "int",
  "a0": { "new_name": "x1" },
  // skipping a1 (preserved from decomp)
  "a2": { "new_name": "x2", "new_type": "u32" },
  "a3": { "new_type": "int*" }
}
```

Note: Keys must match the parameter names in the current decomp output (e.g., `a0`, `a1`, `player`, `flags`). Use `/function/{addr}/decompile` to see current names before modifying.

### GET /function/{addr}/storage

Inspect an exact function entry's return and parameter storage. The response includes the
calling convention, whether custom storage is enabled, Ghidra's storage serialization, and
decoded register/stack pieces. Unlike the other function read endpoints, `addr` must be the
function entry rather than merely an address contained by the function.

### POST /function/signature/custom

Validate or replace a function's complete ordered parameter list using Ghidra custom storage.
The address must be an exact function entry. Omitting `confirm` (or setting it to `false`) is a
read-only validation that returns the current and proposed signatures. Pass `confirm: true` only
after reviewing that result.

**Body:**

```json
{
  "addr": "0x0044d371",
  "parameters": [
    {
      "name": "ship",
      "type": "ShipState *",
      "storage": { "register": "ESI" }
    },
    {
      "name": "jump_progress",
      "type": "float",
      "storage": { "stack_offset": "0x184" },
      "comment": "Optional parameter comment"
    }
  ],
  "return": { "type": "void" },
  "confirm": false
}
```

`parameters` is always the complete ordered list; an empty array is valid for a no-parameter
function. Each parameter must use exactly one storage form:

```json
{ "register": "ESI" }
{ "stack_offset": "0x184" }
{ "serialized": "<Ghidra VariableStorage serialization>" }
```

The serialized form supports compound and other storage layouts and can round-trip the
`serialization` value returned by `/function/{addr}/storage`.

If `return` is omitted, the existing return metadata is preserved. A `void` return must not
specify storage. A non-void return type may omit storage only when the existing return storage is
valid and has the same size; otherwise explicit storage is required.

Validation rejects unknown types/registers, storage/type size mismatches, duplicate parameter
names, and overlapping parameter storage. A confirmed update is performed in one Ghidra
transaction using `CUSTOM_STORAGE` and reports the committed storage afterward.

### GET /function/{addr}/body

Inspect an exact function entry's body ownership. Returns the total byte and range counts,
minimum/maximum addresses, every inclusive address range, and a SHA-256 `body_fingerprint` over
the canonical range list. Use that fingerprint as the stale-state guard for clear and body-update
requests.

### POST /function/clear

Validate or remove one exact function definition. This removes function ownership and
function-specific metadata; it does not clear instructions, data, or references.

```json
{
  "addr": "0x0044d371",
  "expected_name": "Frame_HandlePlayerEscapePodSequence",
  "expected_body_fingerprint": "7e29b657...",
  "confirm": false
}
```

### POST /function/create

Validate or create one function with an explicit, instruction-aligned body. `ranges` are inclusive.
Automatic control-flow body inference is intentionally not supported by this endpoint.

```json
{
  "addr": "0x0044d371",
  "name": "Ship_CompleteHyperspaceExitWithOptionalVelocity",
  "ranges": [
    { "start": "0x0044d371", "end": "0x0044d38d" }
  ],
  "confirm": false
}
```

### POST /function/body/set

Validate or replace one existing function's body while preserving its function metadata.

```json
{
  "addr": "0x0044d371",
  "expected_name": "Frame_HandlePlayerEscapePodSequence",
  "expected_body_fingerprint": "7e29b657...",
  "ranges": [
    { "start": "0x0044d371", "end": "0x0044d38d" }
  ],
  "confirm": false
}
```

### POST /function/restructure

Atomically validate or apply multiple clear, body-update, and create operations. Use this endpoint
for splitting or reorganizing functions so an intermediate failure cannot leave partial body
ownership changes.

```json
{
  "clear": [
    {
      "addr": "0x0044d371",
      "expected_name": "Frame_HandlePlayerEscapePodSequence",
      "expected_body_fingerprint": "7e29b657..."
    }
  ],
  "set_body": [
    {
      "addr": "0x0044aa70",
      "expected_name": "Ship_HandlePlayerShipCore",
      "expected_body_fingerprint": "...",
      "ranges": [
        { "start": "0x0044aa70", "end": "0x0044b11c" }
      ]
    }
  ],
  "create": [
    {
      "addr": "0x0044d371",
      "name": "Ship_CompleteHyperspaceExitWithOptionalVelocity",
      "ranges": [
        { "start": "0x0044d371", "end": "0x0044d38d" }
      ]
    }
  ],
  "confirm": false
}
```

All four write endpoints default to validation-only. Confirmed writes require `confirm: true`.
Validation requires exact function entries, verifies expected names/fingerprints, checks range
ordering and instruction boundaries, requires each entry inside its proposed body, and rejects
final overlaps. A confirmed restructure is one Ghidra transaction and rolls back completely on
failure. Apply custom signatures separately after inspecting newly created functions.

### GET /labels/{addr}

List all symbols at an address with type, namespace, source, and primary/dynamic state.

### POST /label/create, `/label/delete`, `/label/set_primary`

Validation-first label management. Create accepts `{addr, name, primary?, confirm?}` and uses the
containing function namespace when present, otherwise the global namespace. Delete and set-primary
accept `{addr, name, confirm?}` and operate only on `LABEL` symbols, never function symbols.

### GET /instruction/{addr}/flow

Inspect one exact instruction's bytes, raw/effective flow, override, default/current fallthrough,
destinations, owner, and `instruction_fingerprint`.

### POST /instruction/flow

Validate or update instruction flow with a required stale-state fingerprint. `flow_override` is one
of `NONE`, `BRANCH`, `CALL`, `CALL_RETURN`, or `RETURN`. `fallthrough` may be an address, `none`, or
`default` (clear the fallthrough override).

```json
{
  "addr": "0x0044bef7",
  "expected_instruction_fingerprint": "935660f8...",
  "flow_override": "NONE",
  "fallthrough": "default",
  "confirm": false
}
```

### GET /function/{addr}/cfg and `/range/{start}/{end}/cfg`

Return `BasicBlockModel` blocks for an exact function body or address range, including block ranges,
flow types, destinations, and current function ownership.

### POST /full_decompile

Decompile every function in the current program and write one `.c` file per function to disk.

**Body:**

```json
{
  "folder": "/absolute/path/to/output"
}
```

`folder` is required and must be an absolute path. Aliases `out`, `output`, or `path` are also accepted.

**Example:**

```bash
curl -s -X POST "http://127.0.0.1:8166/full_decompile" \
  -H "Content-Type: application/json" \
  -d '{"folder": "/tmp/ghidra_full_decompile"}'
```

**Response:** plain text summary:

```text
ok
folder: /tmp/ghidra_full_decompile
total: 1234
ok: 1220
failed: 14
```

## Important Notes

### URL Encoding

- Invalid syntax error when using regex likely means incorrect URL encoding.
- Example: `name_re=Game.*%7CInit` matches "Game" OR "Init"

### Address Ranges

- `start`/`end` ranges apply to symbols, functions, type methods, xrefs
- Both start and end are required for range filters
- Addresses are typically in hex format (e.g., `0x401000`)

### Struct Modification Safety

- Field modifications require confirmation for:
  - Resizing the struct
  - Creating field overlaps
  - Deleting fields
- Use `/type/{name}/layout` first to verify current state
- Ask the user for confirmation.

### Function Signature Modification Safety

- Always ask the user for confirmation.
