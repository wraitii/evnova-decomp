# Decomp Clustering Workflow

Use clustering to turn large `FUN_*` regions into smaller naming batches that improve future decomp work.

Clustering is not ground truth. Ghidra decompile/disassembly remains the source of truth.

## Inputs

- local Ghidra API server at `http://127.0.0.1:8166`
- a full decompile folder generated from Ghidra
- live caller/callee metadata from the API
- decompile text for globals/strings/features

Relevant tooling:

- [ghidra_api.md](/Users/lancelot/Programming/EVNova-decomp/docs/ghidra_api.md)
- [function_cluster_v2.py](/Users/lancelot/Programming/EVNova-decomp/tools/function_cluster_v2.py)
- [coding_recipe.md](/Users/lancelot/Programming/EVNova-decomp/docs/coding_recipe.md)

## Basic Workflow

1. Generate a full decompile folder.

Use `POST /full_decompile` and write one `.c` file per function to a folder such as `/tmp/ghidra_full_decompile`. There are about 3200 functions total.

2. Run clustering.

Example:

```bash
uv run tools/function_cluster_v2.py /tmp/ghidra_full_decompile analysis/function_clusters_v2
```

Primary outputs:

- `cluster_tree.txt`
- `cluster_tree.json`
- `rename_spans.json`
- `top_edges.json`
- `cluster_report.html`

`cluster_tree.txt` is still the quickest way to browse candidate groups by eye.
Use `cluster_tree.json` when you want to drill into a specific cluster id from the text report.

Current `cluster_tree.json` top-level keys:

- `root_id` - numeric id of the root cluster
- `root` - nested recursive cluster tree
- `clusters_by_id` - flat map from cluster id string to cluster payload
- `clusters` - compatibility alias for `clusters_by_id`
- `address_to_name` - flat map from function address string to current Ghidra name

Useful `jq` patterns:

```bash
# Show a cluster from the id you saw in cluster_tree.txt
jq '.clusters_by_id["143"]' analysis/function_clusters_v2/cluster_tree.json

# Expand addresses in that cluster into address/name rows
jq '
  . as $root
  | $root.clusters_by_id["143"].addresses
  | map({addr: ., name: $root.address_to_name[.]})
' analysis/function_clusters_v2/cluster_tree.json

# Show the cluster summary plus resolved names together
jq '
  . as $root
  | {
      cluster: $root.clusters_by_id["143"],
      names: (
        $root.clusters_by_id["143"].addresses
        | map({addr: ., name: $root.address_to_name[.]})
      )
    }
' analysis/function_clusters_v2/cluster_tree.json
```

3. Pick useful clusters or spans.

Prefer groups that are:

- mostly unnamed
- internally coherent by calls/globals/nearby names
- large enough to matter
- small enough to inspect

Do not assume the biggest residual cluster is a real subsystem. Many are mixed.

4. Sample a few functions manually.

Before delegating or batch-renaming:

- decompile 2-3 representative functions
- inspect nearby named neighbors
- inspect a few callers/callees

This gives a prior such as `NetQuery`, `PNG chunk handling`, `DrawContext`, `Sprite`, or `StreamPath`.

5. Choose one of two rename strategies.

## Strategy A: Function-By-Function Naming

Use this when the cluster looks coherent and the individual function roles are readable.

For each target cluster or span:

- inspect every function yourself or via subagent
- ask for `address -> suggested_name`
- keep confidence notes on weak cases

Then:

- apply high-confidence names directly
- re-check weak names manually
- rename in Ghidra via `POST /function/rename`

This is the best path when you can defend reasonably specific names such as `PngHandleChunk_*` or `NetQuery_Handle*`.

## Strategy B: Broad Prefix Batch Naming

Use this when the cluster family is clear, but many functions are still too vague for specific names.

Workflow:

1. Pick a coherent cluster and assign a conservative family prefix.

Examples:

- `PNG_`
- `NetQuery_`
- `MemOrStream_`
- `OutfitterMenu_`

2. Sample a few unnamed functions from the cluster.

Ask for a light review in the form:

- `address -> broad category name`

Do not force exact semantics. The point is to confirm the family, not to overfit a precise behavior.

3. If the samples support the family, rename the remaining unnamed functions in that cluster with a placeholder-style family prefix:

- `PNG_FUN_00537465`
- `NetQuery_FUN_004f6dc1`
- `MemOrStream_FUN_004fe31a`

4. Spot-check a few renamed addresses in Ghidra.

This strategy works well when:

- the subsystem is obvious
- the cluster is still partly mixed
- you want immediate navigation wins without overclaiming exact semantics

This is especially useful for parser families, network helper blocks, UI menu families, and support libraries.

## Naming Guidance

- Prefer conservative subsystem families over speculative exact names.
- If semantics are clear, use a behavior name.
- If semantics are not clear, use a family prefix plus placeholder suffix.
- Avoid overly specific backend guesses from one sample function.

Examples of acceptable conservative naming:

- `PngHandleChunk_*`
- `NetQuery_*`
- `StreamPath_*`
- `PNG_FUN_*`
- `MemOrStream_FUN_*`

## Common Failure Modes

- Over-trusting large residual clusters.
- Naming from one sample function only.
- Using a UI or file-format prefix that is only weakly suggested by callers.
- Keeping stale metadata while clustering or renaming.

The clustering script should use fresh live Ghidra metadata for each run. In-process caching is fine; persistent stale caches are not.

## Verify And Repeat

After each rename wave:

- query a few addresses back from Ghidra
- confirm the new names appear in listings/decompile headers
- re-run clustering when useful
- peel off the next coherent families

Recommended cadence:

- cluster
- pick 3-4 useful groups
- sample manually
- choose Strategy A or B
- batch rename
- verify
- repeat
