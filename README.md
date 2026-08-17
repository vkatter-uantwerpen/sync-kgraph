# sync-kgraph

`sync-kgraph` implements the synchronize-or-reveal algorithms from the
companion paper as a C23 library and a native Memgraph query module. It treats a
manually mapped knowledge-graph view as a deterministic Mealy automaton and can:

- validate complete transition and observation functions;
- build and persist a generation-scoped pair merge/resolution oracle;
- plan an open-loop word that synchronizes a hypothesis set;
- plan an output-partitioning word that reveals the current hypothesis;
- fall back to exact bounded partition BFS when a pair witness is insufficient;
- explain predicted supports and output branches after every action; and
- monitor localization updates as `CONTINUE`, `REPLAN`, `MODEL_VIOLATION`,
  `STALE_GENERATION`, or `WAIT`.

All algorithm and Memgraph module code is C23. Cypher is used only for schema,
mapping, and example queries. Meson builds the library, CLI, tests, and optional
Memgraph module.

## Build And Test

Core build:

```sh
CC=clang meson setup build -Dmemgraph=disabled
meson compile -C build
meson test -C build --print-errorlogs
./build/sync-kgraph-cli --example warehouse
```

Build the native module against the header installed with the target Memgraph
server. This is preferred over using a header from a different release:

```sh
CC=clang meson setup build-memgraph \
  -Dmemgraph=enabled \
  -Dmemgraph_include_dir=/usr/include/memgraph
meson compile -C build-memgraph
```

The Linux artifact is `build-memgraph/sync.so`; macOS produces the native
shared-module equivalent. Install with Meson, or place that file in the
server's query-module directory and load it:

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
sync.plan_sync
sync.prepare_model
sync.validate_update
```

Run the local integration test with an already installed Memgraph:

```sh
sh scripts/memgraph_local_smoke.sh build-memgraph
```

It starts a separate Memgraph process on a temporary port with private data,
log, and module directories. It never connects to or modifies the normal
Memgraph instance on port 7687.

## Manual View Contract

Run `cypher/install_schema.cypher` once. The application schema is otherwise
untouched. Create dedicated view nodes and relationships for each model:

```cypher
(:SyncModel {
  model, generation, dirty, prepared_generation?
})
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

The mapping is deliberately manual because only the database owner knows how
application entities, orientations, commands, and sensor abstractions form the
automaton. Prefer separate `SyncState` nodes linked by `semantic_ref` or an
application-owned relationship instead of adding `SyncState` to application
nodes. The supplied uninstall script deletes nodes in the Sync-KGraph
namespace.

After creating or changing the view:

1. Set `dirty: true` and advance `generation`, or call
   `sync.mark_dirty(model)` for an existing model.
2. Call `sync.prepare_model(model, materialize_pair_edges)`.
3. Store the returned generation with every plan.
4. Reject or replan work when monitor output is `STALE_GENERATION`.

`prepare_model` validates the strict Mealy model and always persists one
`SyncPair` record for every unordered pair with repetition. Passing `true` also
persists `PAIR_NEXT` and `PAIR_PRE` relationships for inspection. Planning uses
the compact pair records, so edge materialization is optional.

The trigger file is a template, not a generic installed trigger. Adapt its
predicate to the application labels and relationships that feed each model. A
schema-agnostic trigger cannot identify the affected model safely.

## Procedure Interface

```cypher
CALL sync.prepare_model(model, materialize_pair_edges = false)
CALL sync.plan_sync(model, hypotheses, budget)
CALL sync.plan_disambiguate(model, hypotheses, bound, budget)
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
`PARTITION_BFS`, or `NONE`. Dirty or unprepared models are rejected.

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

Expected: nine indexes are created and no data rows are returned. Reuse these
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

### 3. Prepare The Model

```cypher
CALL sync.prepare_model("warehouse", true)
YIELD status, generation, states, actions, outputs, transitions, pairs,
      pair_edges, mergeable_pairs, resolvable_pairs, materialized_pair_edges
RETURN status, generation, states, actions, outputs, transitions, pairs,
       pair_edges, mergeable_pairs, resolvable_pairs, materialized_pair_edges;
```

Expected:

```text
"OK", 1, 5, 4, 4, 20, 15, 60, 15, 15, true
```

There are 15 unordered state pairs with repetition and 60 pair/action edges.
Because edge materialization was enabled, the graph contains 15 `SyncPair`, 60
`PAIR_NEXT`, and 60 `PAIR_PRE` records.

### 4. Plan Synchronization

```cypher
CALL sync.plan_sync(
  "warehouse", ["west_bay:east", "east_bay:west"], 64)
YIELD status, outcome, method, word, length, final_state_key,
      final_support_size, generation
RETURN status, outcome, method, word, length, final_state_key,
       final_support_size, generation;
```

Expected:

```text
"OK", "PLAN", "PAIR_MERGE", ["to_corridor", "go_west"], 2,
"dock:north", 1, 1
```

### 5. Plan Disambiguation

```cypher
CALL sync.plan_disambiguate(
  "warehouse", ["west_bay:east", "east_bay:west"], 1, 64)
YIELD status, outcome, method, word, length, best_support_size,
      worst_support_size, branch_count, homing, generation
RETURN status, outcome, method, word, length, best_support_size,
       worst_support_size, branch_count, homing, generation;
```

Expected:

```text
"OK", "PLAN", "PAIR_RESOLUTION", ["to_corridor"], 1,
1, 1, 2, true, 1
```

The two bays emit different landmark outputs under `to_corridor`, so one
action partitions the initial support into two singleton branches.

### 6. Explain The Synchronizing Word

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

### 7. Validate A Localization Update

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

### 8. Invalidate Old Plans

```cypher
CALL sync.mark_dirty("warehouse") YIELD status, generation
RETURN status, generation;
```

Expected:

```text
"DIRTY", 2
```

Planning is now rejected until `sync.prepare_model("warehouse", false)`
succeeds. A monitor call carrying generation 1 returns:

```text
status: "OK", decision: "STALE_GENERATION", generation: 2
```

## Quality And Releases

```sh
sh scripts/check-format.sh
sh scripts/run-clang-tidy.sh build-tidy
sh scripts/valgrind.sh build-valgrind
sh scripts/coverage.sh build-coverage /usr/include/memgraph
```

CI treats all Clang diagnostics as errors, runs every unit test under Valgrind
with all leak kinds fatal, compiles against multiple Memgraph C API versions,
and runs the full Memgraph integration contract. Coverage includes every
production C source file, including the CLI and Memgraph adapter, and fails
below 75% line coverage.

Release tags publish `linux-x86_64` and native `macos-arm64` archives. Each
archive includes the CLI, C library and header, Memgraph module, Cypher mapping
scripts, Memgraph Lab GSS view, worked example, license, and SHA-256 checksum.
