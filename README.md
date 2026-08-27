# sync-kgraph

`sync-kgraph` exposes synchronize-or-reveal algorithms as a C23 library and a
native Memgraph query module. A manually mapped graph view represents a
deterministic Mealy automaton. The module can then:

- find a word that brings every current hypothesis to one state;
- find a homing word whose outputs distinguish the current hypothesis;
- explain predicted states and output branches after each action;
- validate localization updates and detect stale plans; and
- maintain prepared planning data after transition or observation changes.

Cypher is used only for schema, mapping, and queries. The mapping is manual so
the database owner controls how application entities, actions, and observations
become an automaton.

## Choose A Planning Mode

| Need | API | Preparation | Intended use |
| --- | --- | --- | --- |
| Repeated low-latency plans | `plan_sync`, `plan_disambiguate` | `prepare_model(..., false, false)` | Read-mostly models |
| Frequent small model changes | Cached planners plus `update_cells` | `prepare_model(..., false, true)` | Incrementally maintained models |
| A baseline or one-off plan | `plan_sync_uncached`, `plan_disambiguate_uncached` | None | Rebuilds from the base view on every call |
| Inspect pair transitions in Memgraph Lab | Any cached mode | Set `materialize_pair_edges` to `true` | Visualization only; it does not accelerate planning |

Cached and uncached planners return the same semantic result for the same model
generation. The cached path is optimized for repeated calls; the uncached path
is intentionally retained for controlled comparisons and simple one-off use.

## Build And Install

Build the native module against the C header installed with the target Memgraph
server. Using the server's own header avoids C API version mismatches:

```sh
CC=clang meson setup build-memgraph \
  -Dmemgraph=enabled \
  -Dmemgraph_include_dir=/usr/include/memgraph
meson compile -C build-memgraph
```

The Linux artifact is `build-memgraph/sync.so`; macOS produces the native
shared-module equivalent. Place it in the server's query-module directory, or
install it with Meson, and load it:

```cypher
CALL mg.load("sync");
CALL mg.procedures() YIELD name
WITH name
WHERE name STARTS WITH "sync."
RETURN name ORDER BY name;
```

Expected procedure names:

```text
sync.explain_plan
sync.mark_dirty
sync.plan_disambiguate
sync.plan_disambiguate_uncached
sync.plan_sync
sync.plan_sync_uncached
sync.prepare_model
sync.update_cells
sync.validate_update
```

Prebuilt releases contain Linux x86_64 and native macOS arm64 binaries, the C
header and static library, Cypher scripts, the warehouse example, and the
Memgraph Lab view.

## Map A Model

Run `cypher/install_schema.cypher` once. The application schema is otherwise
untouched. Create dedicated view nodes and relationships for each model:

```cypher
(:SyncModel {model, generation, dirty})
(:SyncState {
  model, state_key, state_id?, semantic_ref?, orientation?
})
(:SyncAction {
  model, action_key, action_id?
})
(:SyncOutput {
  model, output_key, output_id?
})
(:SyncState)-[:SYNC_TRANS {
  model, action_key
}]->(:SyncState)
(:SyncState)-[:SYNC_OBS {
  model, action_key
}]->(:SyncOutput)
```

Keys must be unique within their domain and model. For every state/action pair,
there must be exactly one `SYNC_TRANS` and one `SYNC_OBS`. An observation is the
output emitted for the source state and selected action. Optional numeric IDs
only control stable ordering; keys are the public interface.

`prepared_generation`, `oracle_epoch`, `incremental`,
`pair_edges_materialized`, and `snapshot_token` are managed by the module. The
application should not write them directly.

The mapping is deliberately manual because only the database owner knows how
application entities, orientations, commands, and sensor abstractions form the
automaton. Prefer separate `SyncState` nodes linked by `semantic_ref` or an
application-owned relationship instead of adding `SyncState` to application
nodes. The supplied uninstall script deletes nodes in the Sync-KGraph
namespace.

After creating the view:

1. Set `dirty: true` and advance `generation`, or call
   `sync.mark_dirty(model)` for an existing model.
2. Call `sync.prepare_model(model, materialize_pair_edges, incremental)`.
3. Store the returned generation with every plan.
4. Reject or replan work when monitor output is `STALE_GENERATION`.

`prepare_model` validates the complete Mealy model and creates the derived pair
records used by cached planning. `PAIR_NEXT` and `PAIR_PRE` are optional
inspection relationships; planning never requires them.

The trigger file is a template, not a generic installed trigger. Adapt its
predicate to the application labels and relationships that feed each model. A
schema-agnostic trigger cannot identify the affected model safely. Exclude
writes made by `update_cells` because that procedure handles generation and
repair atomically.

## Procedure API

### Prepared Planning

```cypher
CALL sync.prepare_model(
  model, materialize_pair_edges = false, incremental = false)
CALL sync.plan_sync(model, hypotheses, budget)
CALL sync.plan_disambiguate(model, hypotheses, bound, budget)
```

The cached planners require a clean prepared model. They are the normal choice
when the same model will be queried repeatedly. The first call after a module
restart or cache eviction reports `cache_state: "HYDRATED"`; subsequent calls
normally report `"HOT"`.

The preparation options are independent:

| `materialize_pair_edges` | `incremental` | Behavior |
| --- | --- | --- |
| `false` | `false` | Prepared planning with compact pair records |
| `true` | `false` | Also create `PAIR_NEXT` and `PAIR_PRE` for inspection |
| `false` | `true` | Compact pair records maintained by `update_cells` |
| `true` | `true` | Incremental maintenance plus inspection relationships |

The process cache defaults to 512 MiB. Set `SYNC_KGRAPH_CACHE_MAX_BYTES` to a
decimal byte limit, or `0` to disable retention. Durable pair records remain
available when retention is disabled.

### Incremental Updates

```cypher
CALL sync.prepare_model(model, false, true)
CALL sync.update_cells(model, changes, repair_budget = 0)
```

Each change is a map containing `state_key`, `action_key`, and at least one of
`target_key` or `output_key`. The complete nonempty batch is validated before
anything is written. Duplicate cells and unknown keys are rejected. A batch
containing only current values returns `UNCHANGED` without advancing the model
generation.

Updates change the base view and its prepared records in one transaction.
Incremental mode is optimized for small batches; if the affected region grows
too large, it falls back to an exact full rebuild. Optional `PAIR_NEXT` and
`PAIR_PRE` relationships are updated only when materialization was enabled.

`repair_budget = 0` uses `ceil(pair_count / 4)`. A positive value is an explicit
touch budget. If repair exceeds it, the procedure rebuilds all pair records and
returns `fallback_rebuild: true`. Passing `-1` requests a full rebuild directly;
this is intended for controlled comparisons, not normal operation.

The result reports `maintenance_mode` (`UNCHANGED`, `INCREMENTAL`, or
`FULL_REBUILD`), direct pair deltas, records touched/examined/written, pair edges
examined, database write batches, invalidations, fallback use, and elapsed time.

### Uncached Planning

```cypher
CALL sync.plan_sync_uncached(model, hypotheses, budget)
CALL sync.plan_disambiguate_uncached(model, hypotheses, bound, budget)
```

The uncached planners validate the current base Mealy view and rebuild the pair
oracle in temporary C memory on every call. They work on dirty or unprepared
models and do not create or modify auxiliary records. Use them for a one-off
plan or as a cache-free reference; prepared planning is optimized for repeated
calls.

For the same generation, hypotheses, bound, and budget, cached and uncached
semantic fields are identical.

All four planner procedures additionally return:

| Field | Cached value | Uncached value | Meaning |
| --- | --- | --- | --- |
| `oracle_source` | `PERSISTED` | `RECOMPUTED` | Source of pair witnesses |
| `oracle_builds` | `0` | `1` | Calls to the full oracle builder |
| `cache_state` | `HOT` or `HYDRATED` | `BYPASSED` | Process snapshot state |
| `oracle_rows_loaded` | `0` hot; all pairs hydrated | `0` | Durable rows loaded this call |
| `oracle_load_batches` | `0` hot; hydration batches | `0` | Pair-record queries |
| `oracle_cache_hits` | Snapshot reads | `0` | Witness reads served in C |
| `snapshot_record_reads` | Requested witnesses | `0` | Pair records read by the planner |
| `snapshot_hydration_time_us` | Hydration time or `0` | `0` | Cold snapshot reconstruction |
| `oracle_time_us` | Hydration time or `0` | Rebuild time | Oracle preparation work |
| `total_compute_time_us` | Variable | Variable | Base-view extraction through planner completion |

`planning_time_us` covers only the word planner. `total_compute_time_us` covers
model extraction through planner completion, excluding result encoding, Bolt
transport, and client latency.

After upgrading from a release whose pair records lack `oracle_epoch`, run
`prepare_model` again.

### Supporting API

```cypher
CALL sync.explain_plan(model, generation, hypotheses, word)
CALL sync.validate_update(
  model, generation, hypotheses, word, completed_steps,
  reported_hypotheses, localizer_available = true)
CALL sync.mark_dirty(model)
```

`hypotheses`, `reported_hypotheses`, and returned supports are lists of
`state_key` strings. Words are lists of `action_key` strings. `budget` limits
search expansions; `bound` is the required worst-case output-branch support
size. A bound of one requests a homing word.

Planner calls return an `outcome` of `PLAN`, `ALREADY_SATISFIED`, `NO_PLAN`, or
`RESOURCE_BOUND`, and a `method` of `PAIR_MERGE`, `PAIR_RESOLUTION`,
`PARTITION_BFS`, or `NONE`. Explanation and monitoring retain the prepared-model
lifecycle even when a word was obtained through the uncached API.

The public C API is in `include/sync_kgraph/sync.h`. It exposes the same
automaton builder, pair oracle, planners, explanation visitor, and monitor
without requiring Memgraph.

## Worked Warehouse Example

The example maps two ambiguous bays, two corridor poses, and one dock pose.
Run each numbered file with `mgconsole`, or execute the queries shown below.

### 1. Install The Schema

```sh
mgconsole < cypher/install_schema.cypher
```

Expected: ten indexes are created and no data rows are returned. Reuse these
indexes for every mapped model.

### 2. Load The Manual View

```sh
mgconsole < examples/warehouse/00_reset_and_load.cypher
```

Verify the mapping:

```cypher
MATCH (m:SyncModel {model: "warehouse"})
OPTIONAL MATCH (s:SyncState {model: "warehouse"})
WITH m, count(s) AS states
OPTIONAL MATCH (a:SyncAction {model: "warehouse"})
WITH m, states, count(a) AS actions
OPTIONAL MATCH (o:SyncOutput {model: "warehouse"})
RETURN m.generation AS generation, m.dirty AS dirty,
       states, actions, count(o) AS outputs;
```

Expected:

```text
generation: 1, dirty: true, states: 5, actions: 4, outputs: 4
```

The view contains 20 transitions and 20 observations, one of each per
state/action cell.

### 3. Plan Without The Cache

The freshly loaded model is dirty and has no auxiliary records. Compute a
synchronizing word directly from the base view:

```cypher
CALL sync.plan_sync_uncached(
  "warehouse", ["west_bay:east", "east_bay:west"], 64)
YIELD status, outcome, method, word, length, final_state_key,
      final_support_size, generation, oracle_source, oracle_builds, cache_state
RETURN status, outcome, method, word, length, final_state_key,
       final_support_size, generation, oracle_source, oracle_builds, cache_state;
```

Expected:

```text
"OK", "PLAN", "PAIR_MERGE", ["to_corridor", "go_west"], 2,
"dock:north", 1, 1, "RECOMPUTED", 1, "BYPASSED"
```

Compute the homing word through the same uncached path:

```cypher
CALL sync.plan_disambiguate_uncached(
  "warehouse", ["west_bay:east", "east_bay:west"], 1, 64)
YIELD status, outcome, method, word, length, best_support_size,
      worst_support_size, branch_count, homing, generation,
      oracle_source, oracle_builds, cache_state
RETURN status, outcome, method, word, length, best_support_size,
       worst_support_size, branch_count, homing, generation,
       oracle_source, oracle_builds, cache_state;
```

Expected:

```text
"OK", "PLAN", "PAIR_RESOLUTION", ["to_corridor"], 1,
1, 1, 2, true, 1, "RECOMPUTED", 1, "BYPASSED"
```

Confirm that neither call materialized auxiliary graph data:

```cypher
OPTIONAL MATCH (p:SyncPair {model: "warehouse"})
WITH count(p) AS pairs
OPTIONAL MATCH ()-[r:PAIR_NEXT|PAIR_PRE {model: "warehouse"}]->()
RETURN pairs, count(r) AS pair_relationships;
```

Expected:

```text
pairs: 0, pair_relationships: 0
```

### 4. Prepare The Cached Model

```cypher
CALL sync.prepare_model("warehouse", true)
YIELD status, generation, oracle_epoch, states, actions, outputs, transitions,
      pairs, pair_edges, mergeable_pairs, resolvable_pairs,
      materialized_pair_edges, incremental_enabled
RETURN status, generation, oracle_epoch, states, actions, outputs, transitions,
       pairs, pair_edges, mergeable_pairs, resolvable_pairs,
       materialized_pair_edges, incremental_enabled;
```

Expected:

```text
"OK", 1, 1, 5, 4, 4, 20, 15, 60, 15, 15, true, false
```

There are 15 unordered state pairs with repetition and 60 pair/action edges.
Because edge materialization was enabled, the graph contains 15 `SyncPair`, 60
`PAIR_NEXT`, and 60 `PAIR_PRE` records.

### 5. Plan Synchronization From Persisted Witnesses

```cypher
CALL sync.plan_sync(
  "warehouse", ["west_bay:east", "east_bay:west"], 64)
YIELD status, outcome, method, word, length, final_state_key,
      final_support_size, generation, oracle_source, oracle_builds, cache_state
RETURN status, outcome, method, word, length, final_state_key,
       final_support_size, generation, oracle_source, oracle_builds, cache_state;
```

Expected:

```text
"OK", "PLAN", "PAIR_MERGE", ["to_corridor", "go_west"], 2,
"dock:north", 1, 1, "PERSISTED", 0, "HOT"
```

The semantic columns exactly match the uncached synchronization result. Only
the oracle metadata and timing fields differ.

### 6. Plan Disambiguation From Persisted Witnesses

```cypher
CALL sync.plan_disambiguate(
  "warehouse", ["west_bay:east", "east_bay:west"], 1, 64)
YIELD status, outcome, method, word, length, best_support_size,
      worst_support_size, branch_count, homing, generation,
      oracle_source, oracle_builds, cache_state
RETURN status, outcome, method, word, length, best_support_size,
       worst_support_size, branch_count, homing, generation,
       oracle_source, oracle_builds, cache_state;
```

Expected:

```text
"OK", "PLAN", "PAIR_RESOLUTION", ["to_corridor"], 1,
1, 1, 2, true, 1, "PERSISTED", 0, "HOT"
```

The two bays emit different landmark outputs under `to_corridor`, so one
action partitions the initial support into two singleton branches. This result
also exactly matches the uncached homing result.

### 7. Explain The Synchronizing Word

```cypher
CALL sync.explain_plan(
  "warehouse", 1,
  ["west_bay:east", "east_bay:west"],
  ["to_corridor", "go_west"])
YIELD step, action, predicted_hypotheses, output_trace, branch_hypotheses
RETURN step, action, predicted_hypotheses, output_trace, branch_hypotheses
ORDER BY step, output_trace;
```

Expected rows:

```text
0, "", [west_bay:east, east_bay:west], [],
   [west_bay:east, east_bay:west]
1, "to_corridor", [corridor_w:east, corridor_e:west],
   [west_landmark], [corridor_w:east]
1, "to_corridor", [corridor_w:east, corridor_e:west],
   [east_landmark], [corridor_e:west]
2, "go_west", [dock:north], [west_landmark, dock], [dock:north]
2, "go_west", [dock:north], [east_landmark, dock], [dock:north]
```

### 8. Validate A Localization Update

```cypher
CALL sync.validate_update(
  "warehouse", 1,
  ["west_bay:east", "east_bay:west"],
  ["to_corridor", "go_west"], 1,
  ["corridor_w:east", "corridor_e:west"], true)
YIELD status, decision, expected_hypotheses, unexpected_hypotheses, generation
RETURN status, decision, expected_hypotheses, unexpected_hypotheses, generation;
```

Expected:

```text
"OK", "CONTINUE", [corridor_w:east, corridor_e:west], [], 1
```

Reporting only one expected corridor returns `REPLAN`; reporting `dock:north`
at this step returns `MODEL_VIOLATION`; passing an unavailable localizer returns
`WAIT`.

### 9. Invalidate Old Plans

```cypher
CALL sync.mark_dirty("warehouse") YIELD status, generation
RETURN status, generation;
```

Expected:

```text
"DIRTY", 2
```

Cached planning is now rejected until
`sync.prepare_model("warehouse", false)` succeeds. Uncached planning remains
available against the current base view and returns generation 2. A monitor
call carrying generation 1 returns:

```text
status: "OK", decision: "STALE_GENERATION", generation: 2
```

### 10. Prepare And Update Incrementally

Prepare the dirty generation in compact incremental mode. This keeps the
derived graph compact and enables `update_cells`:

```cypher
CALL sync.prepare_model("warehouse", false, true)
YIELD status, generation, oracle_epoch, pairs, pair_edges,
      materialized_pair_edges, incremental_enabled
RETURN status, generation, oracle_epoch, pairs, pair_edges,
       materialized_pair_edges, incremental_enabled;
```

Expected:

```text
"OK", 2, 2, 15, 60, false, true
```

Change one observation cell:

```cypher
CALL sync.update_cells("warehouse", [{
  state_key: "east_bay:west",
  action_key: "to_wall",
  output_key: "east_landmark"
}], 15)
YIELD status, maintenance_mode, generation, oracle_epoch, changed_cells,
      direct_pair_edges, fallback_rebuild
RETURN status, maintenance_mode, generation, oracle_epoch, changed_cells,
       direct_pair_edges, fallback_rebuild;
```

Expected:

```text
"UPDATED", "INCREMENTAL", 3, 2, 1, 5, false
```

The five direct deltas are the `to_wall` entries for the five unordered pairs
containing `east_bay:west`. The epoch stays at 2 while generation advances, and
no pair relationships are created. Repeating the same value is a no-op:

```cypher
CALL sync.update_cells("warehouse", [{
  state_key: "east_bay:west",
  action_key: "to_wall",
  output_key: "east_landmark"
}], 15)
YIELD status, generation, oracle_epoch, changed_cells, direct_pair_edges
RETURN status, generation, oracle_epoch, changed_cells, direct_pair_edges;
```

Expected:

```text
"UNCHANGED", 3, 2, 0, 0
```

Cached and uncached homing calls still return `["to_corridor"]` with generation
3. An invalid key aborts the whole batch and leaves generation, base cells, and
the oracle unchanged.

## Developer Documentation

See [HACKING.md](HACKING.md) for the C snapshot architecture, cache and update
lifecycle diagrams, correctness invariants, ablation benchmarks, quality gates,
coverage policy, and release process.
