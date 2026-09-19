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

### POST /function/synthetic-decompile

Decompile a CFG region as a read-only synthetic function. `entry` and every
member of `stops` may be an address or a uniquely resolving label. CFG traversal
starts at `entry` and stops before each boundary. No function records, bodies,
bytes, labels, or other program metadata are changed.

The region's **signature is derived without decompiling anything**. A liveness
pass over raw instruction pcode finds the storage the region reads before writing
(its parameters) and the storage it writes and leaves unread on the way to a
boundary (its return value). Stack slots come from the stack references already
attached to the listing, rebased from the parent's frame to the region entry.
This matters most on exactly the functions these views are for: on a 40KB parent
whose own decompilation takes 38 seconds, a region view returns in under a
second, with the same signature either way.

The decompiler is used only to put better **names and types** on storage that has
already been identified, and never changes which storage is selected. Control it
with `names`:

- `off` - dataflow names only (`ebp`, `stack_40`), and no decompilation at all
- `auto` (default) - try the parent for five seconds, fall back silently
- `full` - allow the full 60-second parent pass

Naming needs both the parent's decompilation, which holds the semantics, and a
region-local draft, which holds the physical storage - a `__cdecl` pointer
parameter lives at a stack slot in the parent but in a register in the region.
Both are skipped entirely under `off` or when the parent times out, which costs
names and never correctness.

Parameters:

- `entry` (alias `start`) - required basic-block entry address or unique label
- `stops` (alias `stop`) - optional JSON array or comma-separated boundary list;
  traversal excludes these blocks and each supplied stop must be reachable. Omit it
  for a region that runs to the parent's own return - see below
- `name` - optional synthetic function name
- `names` - optional `off`, `auto` (default) or `full`
- `inputs` - optional storage overrides keyed by register (for example `ESI`) or
  `stack:<offset>`, each containing `name` and `type`; these replace inference
  for the same storage
- `output` - optional `auto` (default), `none`, or a storage name from
  `output_candidates` to force that storage as the return value
- `output_type` - optional type name overriding the inferred return type
- `exits` - optional `auto` (default), `on` or `off`; see multi-exit regions below
- `trace` - optional Boolean, default `false`; reports how each op of the region
  paired up with the parent's, for diagnosing a type or name that did not carry
  over
- `max_blocks` - optional block budget, 1 through 5000, default `500`
- `force_infer` - deprecated alias for `"names":"full"`

The reply reports `inputs` and `output_candidates` alongside the C, so you can
see what was recovered and override it. Only *plausible* output storage is
selected automatically - callee-saved registers or the ABI return register,
flagged `"plausible": true` - because a dirty volatile scratch register is
usually just residue from the region's last call. Force one of the others with
`output` when you know better.

The return value matters more than it looks. Without one, a region whose only
product is a live-out - a predicate chain setting a flag register, say -
decompiles to an empty `void` body, because nothing it computes is observable.

**Regions that return.** Leaving through the parent's own `ret` is a boundary
like any other, and the only one available to a region that runs to the end of
its parent. Give no `stops` at all and the region ends at those returns: the
reply reports `"returns": true`, and the view inherits the parent's return type
and storage, since nothing is live out past a return. This is what lets a tail
be lifted out whole - on a 40KB parent, collapsing the tail leaves a 167-line
skeleton that decompiles in 0.3s instead of 49s.

**Multi-exit regions.** A region with several boundaries carries a product the
storage analysis cannot see: which one it took. With more than one the view
returns that instead - each boundary becomes `return <n>;`, and the reply's
`exit_codes` maps each `n` to its stop address and label, with the parent's
return appearing as `"kind": "return"` when the region can also end that way. Without it the C ends
in several returns the reader cannot tell apart. Control it with `exits`:

- `auto` (default) - the exit code wins whenever there is more than one boundary
- `on` - same, and rejected together with an explicit `output`
- `off` - no exit code; the return goes back to the recovered data output

A data live-out is not lost when the exit code takes the return, only unreturned:
it still shows in `output_candidates`, and `exits:"off"` returns it instead.

The exit code returns in the ABI return register, so a region that also leaves a
call result there shows a `scratch_<reg>` local. That is the register's entry
value, not a parameter the recovery missed: a call's return register counts as
clobbered, and passing it in would hand the region whatever happened to be
there.

Known limits: only one live-out becomes the return, so a region producing two
values silently drops the second; stack live-outs are detected but never selected
automatically; and a value left in the return register by the region's last call
is not treated as an output unless forced.

```bash
curl -s -X POST "http://127.0.0.1:8166/function/synthetic-decompile" \
  -H "Content-Type: application/json" \
  -d '{
    "entry":"LAB_00401530",
    "stops":["LAB_0040132b"],
    "name":"View_DispatchShipAiBehavior",
    "names":"full",
    "inputs":{
      "EBP":{"name":"ship","type":"ShipState *"},
      "stack:6":{"name":"mission_attack","type":"bool"}
    }
  }'
```

### POST /function/parent-view

Show a parent function with chosen CFG regions collapsed into calls to their
synthetic views, and (by default) each of those views decompiled alongside it.
Read-only: no function records, bodies, bytes, labels or other program metadata
are changed.

Each region's entry instruction is replaced by `result = View_X(...)` followed by
the branch to whichever boundary the result names, so the region's own
instructions become unreachable and are never decompiled into the parent. The
signature on that call is the one `/function/synthetic-decompile` recovers, from
the same dataflow pass, so the parent shows the region's real arguments and
return. Regions with several stops are always given exit codes, because the
parent needs them to pick the boundary to branch to.

What is left is the parent's own skeleton - the control flow between the regions
- plus one self-contained function per region.

Parameters:

- `function` (alias `addr`) - required exact function name or contained address
- `regions` - required JSON array of `{entry, stops?, name?, inputs?, output?,
  output_type?, names?, max_blocks?}` objects, each taking the same fields as
  `/function/synthetic-decompile`; regions may not overlap, and a region with no
  `stops` collapses into a tail call
- `names` - optional default `off`/`auto`/`full` for every region
- `bodies` - optional Boolean, default `true`; `false` returns the overview alone
- `plan` - optional Boolean, default `false`; `true` reports the regions and
  their recovered signatures without decompiling anything, so a split can be
  checked before paying for it
- `timeout` - optional parent decompile timeout in seconds, 1 through 600,
  default `60`
- `max_blocks` - optional default block budget for every region

The reply reports each region's name, entry, stops (with `exit_code` where
present), recovered `signature`, `decompile_ms` and `c`, the parent's `c`, and a
`timing` breakdown. A branch that lands inside a region rather than on its entry
cannot go through the call, so that path is decompiled inline and the region
body appears in the parent anyway. That is a property of the CFG, so it is
reported as `reentered` (`region`, `block`, `from`) before any decompiling -
`plan:true` shows it, and the fix is to split the region at the block named.

The parent's decompilation, which the naming overlay needs, is shared across
every region in one request rather than fetched per region.

```bash
curl -s -X POST "http://127.0.0.1:8166/function/parent-view" \
  -H "Content-Type: application/json" \
  -d '{
    "function":"Ship_UpdateShipAI",
    "regions":[
      {"entry":"00401530","stops":["0040132b"],"name":"View_DispatchBehavior"},
      {"entry":"004012ae","stops":["00401323","004014c2","004014b4"],
       "name":"View_MaybeRetarget"}
    ]
  }'
```

### POST /function/synthetic-suggestions

Propose a way to break a function up. The reply leads with a **plan**: a set of
non-overlapping regions that between them cover as much of the parent as they
can, each ready to paste into `/function/parent-view` or
`/function/synthetic-decompile`.

Candidates are the regions between a block and its **immediate post-dominator**,
taken from real dominator and post-dominator trees rather than from a search for
where two branch arms happen to meet. Such a region is single-entry and
single-exit by construction, so the parent can replace it with one call and one
branch, and nothing else in the parent falls into its middle.

Being a valid region is not the same as being a useful one, so every candidate is
then run through the same dataflow pass that gives a view its signature. A
suggestion therefore reports the parameters and return value the split would
actually produce - the thing that decides whether a region reads as a function or
as a slice through the middle of the parent's state - and is scored on them. A
region reading two registers scores well; one reading nine does not.

Raising `max_regions` only ever appends. The greedy runs down one fixed ranking,
so a longer plan is the shorter one plus more - a region is never dropped or
reshuffled when the limit goes up - which is why the limit is effectively off by
default and `min_score` is the real quality control.

Output is one line per region, in address order under the plan header:

```
-- plan: 16 regions, 17881 of 39831 bytes (45%), Ship_HandlePlayerShipCore keeps 21950 bytes
  0044b037 (PlayerTick_HyperspaceExitGate) -> [0044b23f]  17 blocks, 611 bytes, 13 calls  |  EDI(ESI) +1 scratch  |  1.29
```

`EDI(ESI)` is the recovered signature: the region returns `EDI` and takes `ESI`.
`+1 scratch` counts further live-outs the parent would have to carry by hand, and
counts against the score. The parenthesized label is omitted when an address has
no primary label or only a default `LAB_<address>` one.

Regions that overlap the plan are listed after it under `-- alternatives`, ranked
by score; they are choices to make *instead of* a plan entry, not as well as one.
Set `alternatives:false` for the plan alone.

A region whose paths never reconverge ends by returning from the parent and is
shown with a `[return]` boundary. A region running from the function's own entry
is the whole function, and is not suggested.

Where a rare path escapes early - a bail-out to shared cleanup, say -
reconvergence is a long way off, and the single-exit region is most of the
function or does not exist at all. For those a second candidate is built from
everything the block *dominates*, which is single-entry whatever its shape, and
is reported with several stops for `/function/parent-view` to give exit codes to.
That costs the parent a switch to read, so each boundary past the first counts
against the score and such a region has to be a distinctly better unit to be
proposed at all: on a 40KB parent one of 47 plan entries was multi-exit. Set
`max_exits:1` to leave them out entirely.

Regions sharing an exit are the same split proposed a block or two apart, so only
the widest of each such chain is reported. Metrowerks and similar compilers reach
distant labels through one-instruction jump islands parked at the end of the
function; those are spliced out of the graph, so a short backward branch is not
reported as a 38KB span and an island is never offered as a start.

Parameters:

- `function` (alias `addr`) - required exact function name or contained address
- `max_regions` - optional integer from 1 through 500, default `500`; a cap on
  plan length, off by default because raising it only appends
- `min_score` - optional number, default `0.35`; the quality floor for the plan,
  and the setting that actually decides how much is proposed. Lowering it trades
  coherence for coverage, with diminishing returns: on a 40KB parent `0.35` gives
  48 regions over 70% of the function and `0` gives 67 over 81%
- `max_exits` - optional integer from 1 through 8, default `3`; `1` proposes only
  regions with a single continuation
- `alternatives` - optional Boolean, default `true`
- `named_labels_as_starts` - optional Boolean, default `true`; also consider
  blocks carrying a non-dynamic primary label, not only branch and merge points
- `min_blocks` - optional integer from 2 through 100, default `4`
- `min_bytes` - optional integer, default `96`
- `max_blocks` - optional integer from `min_blocks` through 5000, default `500`,
  matching what a view will accept
- `max_depth` - optional integer from 0 through 50, default `2`; how many
  alternatives to list

```bash
curl -s -X POST "http://127.0.0.1:8166/function/synthetic-suggestions" \
  -H "Content-Type: application/json" \
  -d '{"function":"Ship_UpdateShipAI"}'
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

Find instructions that use a specific scalar value, and report **how** each one
uses it. Results are grouped by containing function, so a function holding a
dozen sites is listed - and decompiled - once.

`kind` is **required**. Matching is done on the instruction's pcode, not on its
disassembly text, so `kind=read` means a value genuinely loaded from
`[base + op]` rather than any line that happens to contain "mov".

- `read` - value loaded from `[base + op]`
- `write` - value stored to `[base + op]`
- `call` - indirect call through `[base + op]` (vtable / function pointer)
- `addr` - address `[base + op]` taken but not dereferenced (LEA)
- `imm` - `op` appears as a plain immediate, not a displacement (noisy)
- `any` - all of the above

Combine with commas: `kind=read,write`.

**Note:** this searches a bare offset, so it still finds every struct that
happens to use it. When you know the type, prefer
`/struct/{name}/field/{field}/xrefs` below, which resolves the base type.

**Parameters:**

- `op` - operand scalar value to search for (decimal or 0x-prefixed hex)
- `kind` - required; see above
- `filter?` - regex further filtering the instruction text
- `context_filter?` - regex over nearby disassembly (5 lines either side by default)
- `start?` / `end?` - restrict to functions whose entry point is in this range
- `limit?` - maximum number of sites, default 400
- `verbose?` - `true` to print full plate comments rather than their first line

**Examples:**

```bash
# Who writes field 0xc90a?
curl -s "http://127.0.0.1:8166/operand_search?op=0xc90a&kind=write"

# Virtual calls through vtable slot 0x10
curl -s "http://127.0.0.1:8166/operand_search?op=0x10&kind=call"

# Everything, as the endpoint used to behave
curl -s "http://127.0.0.1:8166/operand_search?op=0x10&kind=any"
```

### GET /operand_search/disasm

As `/operand_search`, with disassembly context around each site.
Adds `before?`/`after?` (default 5/0).

### GET /operand_search/decomp

As `/operand_search`, with decompiled context around each site. Each function is
decompiled once regardless of how many sites it holds.
Adds `before?`/`after?` (default 4/2).

```bash
curl -s "http://127.0.0.1:8166/operand_search/decomp?op=0xc90a&kind=read,write"
```

### GET /struct/{name}/field/{offset|fieldname}/xrefs

Find uses of one **struct field**, resolving the base type rather than matching a
bare offset. This is the endpoint to reach for when working out what a field
means.

No decompilation is involved, so it costs about a second on a large program and
needs no warm cache. It works by propagating the types already in the program
database - parameter and local signatures, and globals typed as the struct or an
array of it - forward over raw instruction pcode, flow-sensitively over the CFG.
Array walks are recovered from the stride even when the array's own symbol is
untyped: a register scaled by `sizeof(struct)`, or stepped by it in a loop, can
only be indexing this struct.

The field may be named or given as an offset (decimal or 0x-hex); an offset
inside a field resolves to the field containing it.

**Parameters:**

- `kind?` - as `/operand_search`, default `read,write,call,addr`
- `limit?` - maximum number of sites, default 400
- `include_untyped?` - `true` to also list accesses at the same offset whose base
  could not be proved to be this struct. Useful as a sanity check, and to catch
  the residue the analysis misses
- `debug?` - `true` to report the seeds and per-candidate state, for diagnosing a
  use that was not attributed

Each site is reported with its kind and the root the pointer came from
(`param`, `local`, `array[i]` or `global`).

**Examples:**

```bash
curl -s "http://127.0.0.1:8166/struct/ShipState/field/escort_command_code/xrefs"

# Only the writes - the fastest way to learn what sets a field
curl -s "http://127.0.0.1:8166/struct/ShipState/field/0xc90a/xrefs?kind=write"
```

The same offset means different things in different structs, which is exactly
what this resolves: `op=0x1c` alone matches over 400 sites program-wide, while
`ShipState+0x1c` (`pos_y`) has 149 and `ShipClassDef+0x1c` has none.

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

Only fields explicitly present in the request are committed. In particular, renaming one parameter
does not commit the decompiler's inferred types for the return value or other parameters.

### POST /function/signature/reset

Remove a committed function prototype so Ghidra can infer it again. This preserves the function
name, namespace, comments, locals, and calling convention, but removes all parameters and resets the
return type/signature source to defaults.

**Body:** `{addr: "00421f50", confirm: true}`

### GET /labels/{addr}

List all symbols at an address with type, namespace, source, and primary/dynamic state.

### POST /label/create, `/label/delete`, `/label/set_primary`

Validation-first label management. Create accepts `{addr, name, primary?, confirm?}` and uses the
containing function namespace when present, otherwise the global namespace. Delete and set-primary
accept `{addr, name, confirm?}` and operate only on `LABEL` symbols, never function symbols.

### GET /function/{addr}/cfg and `/range/{start}/{end}/cfg`

Compact `BasicBlockModel` output for the body of the function containing `addr` (like the other
function read actions), or for an explicit address range. First line is a summary; each following
line is one basic block: `start-end type -> <dest> <flow> ...`. Flow labels: `fall`
(fall-through), `jcc` (conditional), `jmp` (unconditional), `jmp*` (computed), `call`/`call*`
(direct/computed call targets), `term` (terminator, e.g. return or jump-thunk).

### GET /range/{addr}/disasm

Disassemble the basic block containing `addr` (as reported by the CFG endpoints). Use
`/range/{start}/{end}/disasm` for arbitrary ranges.

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
