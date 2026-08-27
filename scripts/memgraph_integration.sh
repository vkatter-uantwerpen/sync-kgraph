#!/usr/bin/env sh
set -eu

mode="${1:-local}"
container="${2:-}"
host="${MEMGRAPH_HOST:-127.0.0.1}"
port="${MEMGRAPH_PORT:-7687}"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

run_query() {
  if [ "$mode" = "docker" ]; then
    printf '%s\n' "$1" | docker exec -i "$container" mgconsole \
      --host=127.0.0.1 \
      --port=7687 \
      --output_format=csv \
      --no_history
  else
    printf '%s\n' "$1" | mgconsole \
      --host="$host" \
      --port="$port" \
      --output_format=csv \
      --no_history
  fi
}

run_file() {
  if [ "$mode" = "docker" ]; then
    docker exec -i "$container" mgconsole \
      --host=127.0.0.1 \
      --port=7687 \
      --output_format=csv \
      --no_history <"$1"
  else
    mgconsole \
      --host="$host" \
      --port="$port" \
      --output_format=csv \
      --no_history <"$1"
  fi
}

assert_pass() {
  name="$1"
  query="$2"
  output=$(run_query "$query") || {
    printf '%s\n' "integration check failed to execute: $name" >&2
    exit 1
  }
  if ! printf '%s\n' "$output" | grep -q 'PASS'; then
    printf '%s\n' "integration check failed: $name" "$output" >&2
    exit 1
  fi
}

expect_failure() {
  name="$1"
  query="$2"
  if run_query "$query" >/dev/null 2>&1; then
    printf '%s\n' "integration query unexpectedly succeeded: $name" >&2
    exit 1
  fi
}

case "$mode" in
local)
  ;;
docker)
  if [ -z "$container" ]; then
    echo "docker mode requires a container name" >&2
    exit 2
  fi
  ;;
*)
  echo "usage: $0 [local | docker <container>]" >&2
  exit 2
  ;;
esac

run_query 'CALL mg.load("sync");' >/dev/null
run_file "$root/examples/warehouse/00_reset_and_load.cypher" >/dev/null

assert_pass "registered procedures" '
CALL mg.procedures() YIELD name
WITH name
WHERE name STARTS WITH "sync."
WITH count(name) AS procedures
RETURN CASE WHEN procedures = 9 THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "uncached synchronization before preparation" '
CALL sync.plan_sync_uncached(
  "warehouse", ["west_bay:east", "east_bay:west"], 64)
YIELD status, outcome, method, word, length, final_state_key,
      final_support_size, generation, oracle_source, oracle_builds,
      oracle_rows_loaded, oracle_load_batches, oracle_cache_hits,
      cache_state, snapshot_record_reads, oracle_time_us,
      snapshot_hydration_time_us, total_compute_time_us
RETURN CASE WHEN status = "OK" AND outcome = "PLAN"
                 AND method = "PAIR_MERGE"
                 AND word = ["to_corridor", "go_west"] AND length = 2
                 AND final_state_key = "dock:north"
                 AND final_support_size = 1 AND generation = 1
                 AND oracle_source = "RECOMPUTED" AND oracle_builds = 1
                 AND cache_state = "BYPASSED" AND snapshot_record_reads = 0
                 AND oracle_rows_loaded = 0 AND oracle_load_batches = 0
                 AND oracle_cache_hits = 0
                 AND snapshot_hydration_time_us = 0
                 AND oracle_time_us >= 0 AND total_compute_time_us >= oracle_time_us
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "uncached disambiguation before preparation" '
CALL sync.plan_disambiguate_uncached(
  "warehouse", ["west_bay:east", "east_bay:west"], 1, 64)
YIELD status, outcome, method, word, length, best_support_size,
      worst_support_size, branch_count, homing, generation,
      oracle_source, oracle_builds, oracle_rows_loaded,
      oracle_load_batches, oracle_cache_hits, cache_state,
      snapshot_record_reads, snapshot_hydration_time_us
RETURN CASE WHEN status = "OK" AND outcome = "PLAN"
                 AND method = "PAIR_RESOLUTION" AND word = ["to_corridor"]
                 AND length = 1 AND best_support_size = 1
                 AND worst_support_size = 1 AND branch_count = 2
                 AND homing AND generation = 1
                 AND oracle_source = "RECOMPUTED" AND oracle_builds = 1
                 AND cache_state = "BYPASSED" AND snapshot_record_reads = 0
                 AND oracle_rows_loaded = 0 AND oracle_load_batches = 0
                 AND oracle_cache_hits = 0
                 AND snapshot_hydration_time_us = 0
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "uncached planning leaves auxiliary graph empty" '
MATCH (m:SyncModel {model: "warehouse"})
OPTIONAL MATCH (p:SyncPair {model: "warehouse"})
WITH m, count(p) AS pairs
OPTIONAL MATCH ()-[r:PAIR_NEXT|PAIR_PRE {model: "warehouse"}]->()
WITH m, pairs, count(r) AS edges
RETURN CASE WHEN m.dirty AND m.prepared_generation IS NULL
                 AND pairs = 0 AND edges = 0
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "prepare model" '
CALL sync.prepare_model("warehouse", true)
YIELD status, generation, oracle_epoch, states, actions, outputs, transitions,
      pairs, pair_edges, mergeable_pairs, resolvable_pairs,
      materialized_pair_edges, incremental_enabled
RETURN CASE WHEN status = "OK" AND generation = 1 AND oracle_epoch = 1 AND states = 5
                 AND actions = 4 AND outputs = 4 AND transitions = 20
                 AND pairs = 15 AND pair_edges = 60
                 AND mergeable_pairs = 15 AND resolvable_pairs = 15
                 AND materialized_pair_edges AND NOT incremental_enabled
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "materialized pair records" '
MATCH (p:SyncPair {model: "warehouse", oracle_epoch: 1})
WITH count(p) AS pairs
OPTIONAL MATCH (:SyncPair {model: "warehouse", oracle_epoch: 1})
               -[n:PAIR_NEXT {model: "warehouse", oracle_epoch: 1}]->
               (:SyncPair {model: "warehouse", oracle_epoch: 1})
WITH pairs, count(n) AS next_edges
OPTIONAL MATCH (:SyncPair {model: "warehouse", oracle_epoch: 1})
               -[p:PAIR_PRE {model: "warehouse", oracle_epoch: 1}]->
               (:SyncPair {model: "warehouse", oracle_epoch: 1})
WITH pairs, next_edges, count(p) AS pre_edges
RETURN CASE WHEN pairs = 15 AND next_edges = 60 AND pre_edges = 60
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "synchronization plan" '
CALL sync.plan_sync(
  "warehouse", ["west_bay:east", "east_bay:west"], 64)
YIELD status, outcome, method, word, length, final_state_key,
      final_support_size, generation, oracle_source, oracle_builds,
      oracle_rows_loaded, oracle_load_batches, oracle_cache_hits,
      cache_state, snapshot_record_reads, oracle_time_us,
      snapshot_hydration_time_us, total_compute_time_us
RETURN CASE WHEN status = "OK" AND outcome = "PLAN"
                 AND method = "PAIR_MERGE"
                 AND word = ["to_corridor", "go_west"] AND length = 2
                 AND final_state_key = "dock:north"
                 AND final_support_size = 1 AND generation = 1
                 AND oracle_source = "PERSISTED" AND oracle_builds = 0
                 AND cache_state = "HOT" AND oracle_rows_loaded = 0
                 AND oracle_load_batches = 0 AND snapshot_record_reads > 0
                 AND oracle_cache_hits = snapshot_record_reads
                 AND snapshot_hydration_time_us = 0
                 AND oracle_time_us >= 0 AND total_compute_time_us >= oracle_time_us
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "force a cold snapshot lookup" '
MATCH (m:SyncModel {model: "warehouse"})
SET m.snapshot_token = "integration-hydration-epoch-1"
RETURN CASE WHEN m.snapshot_token = "integration-hydration-epoch-1"
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "disambiguation plan" '
CALL sync.plan_disambiguate(
  "warehouse", ["west_bay:east", "east_bay:west"], 1, 64)
YIELD status, outcome, method, word, length, best_support_size,
      worst_support_size, branch_count, homing, generation,
      oracle_source, oracle_builds, oracle_rows_loaded,
      oracle_load_batches, oracle_cache_hits, cache_state,
      snapshot_record_reads, snapshot_hydration_time_us
RETURN CASE WHEN status = "OK" AND outcome = "PLAN"
                 AND method = "PAIR_RESOLUTION" AND word = ["to_corridor"]
                 AND length = 1 AND best_support_size = 1
                 AND worst_support_size = 1 AND branch_count = 2
                 AND homing AND generation = 1
                 AND oracle_source = "PERSISTED" AND oracle_builds = 0
                 AND cache_state = "HYDRATED" AND oracle_rows_loaded = 15
                 AND oracle_load_batches = 1 AND snapshot_record_reads > 0
                 AND oracle_cache_hits = snapshot_record_reads
                 AND snapshot_hydration_time_us >= 0
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "plan explanation" '
CALL sync.explain_plan(
  "warehouse", 1,
  ["west_bay:east", "east_bay:west"],
  ["to_corridor", "go_west"])
YIELD step, branch_hypotheses
WITH count(*) AS rows,
     sum(CASE WHEN step = 0 THEN 1 ELSE 0 END) AS initial_rows,
     sum(CASE WHEN step = 1 THEN 1 ELSE 0 END) AS first_step_rows,
     sum(CASE WHEN step = 2 AND branch_hypotheses = ["dock:north"]
              THEN 1 ELSE 0 END) AS terminal_rows
RETURN CASE WHEN rows = 5 AND initial_rows = 1 AND first_step_rows = 2
                 AND terminal_rows = 2
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "monitor continue" '
CALL sync.validate_update(
  "warehouse", 1,
  ["west_bay:east", "east_bay:west"],
  ["to_corridor", "go_west"], 1,
  ["corridor_w:east", "corridor_e:west"], true)
YIELD status, decision, expected_hypotheses, unexpected_hypotheses, generation
RETURN CASE WHEN status = "OK" AND decision = "CONTINUE"
                 AND size(expected_hypotheses) = 2
                 AND size(unexpected_hypotheses) = 0 AND generation = 1
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "monitor replan" '
CALL sync.validate_update(
  "warehouse", 1,
  ["west_bay:east", "east_bay:west"],
  ["to_corridor", "go_west"], 1,
  ["corridor_w:east"], true)
YIELD status, decision, expected_hypotheses, unexpected_hypotheses
RETURN CASE WHEN status = "OK" AND decision = "REPLAN"
                 AND size(expected_hypotheses) = 2
                 AND size(unexpected_hypotheses) = 0
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "monitor model violation" '
CALL sync.validate_update(
  "warehouse", 1,
  ["west_bay:east", "east_bay:west"],
  ["to_corridor", "go_west"], 1,
  ["dock:north"], true)
YIELD status, decision, unexpected_hypotheses
RETURN CASE WHEN status = "OK" AND decision = "MODEL_VIOLATION"
                 AND unexpected_hypotheses = ["dock:north"]
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "monitor wait" '
CALL sync.validate_update(
  "warehouse", 1,
  ["west_bay:east", "east_bay:west"],
  ["to_corridor", "go_west"], 1, [], false)
YIELD status, decision, expected_hypotheses, unexpected_hypotheses
RETURN CASE WHEN status = "OK" AND decision = "WAIT"
                 AND size(expected_hypotheses) = 2
                 AND size(unexpected_hypotheses) = 0
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "monitor stale generation" '
CALL sync.validate_update(
  "warehouse", 0,
  ["west_bay:east", "east_bay:west"],
  ["to_corridor", "go_west"], 1, [], false)
YIELD status, decision, expected_hypotheses, unexpected_hypotheses, generation
RETURN CASE WHEN status = "OK" AND decision = "STALE_GENERATION"
                 AND size(expected_hypotheses) = 0
                 AND size(unexpected_hypotheses) = 0 AND generation = 1
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "mark dirty" '
CALL sync.mark_dirty("warehouse") YIELD status, generation
RETURN CASE WHEN status = "DIRTY" AND generation = 2
            THEN "PASS" ELSE "FAIL" END AS result;'

expect_failure "planning rejects dirty model" '
CALL sync.plan_sync(
  "warehouse", ["west_bay:east", "east_bay:west"], 64)
YIELD status RETURN status;'

assert_pass "uncached planning accepts dirty model" '
CALL sync.plan_sync_uncached(
  "warehouse", ["west_bay:east", "east_bay:west"], 64)
YIELD status, outcome, word, generation, oracle_source, oracle_builds
RETURN CASE WHEN status = "OK" AND outcome = "PLAN"
                 AND word = ["to_corridor", "go_west"] AND generation = 2
                 AND oracle_source = "RECOMPUTED" AND oracle_builds = 1
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "old plan is stale after model change" '
CALL sync.validate_update(
  "warehouse", 1,
  ["west_bay:east", "east_bay:west"],
  ["to_corridor", "go_west"], 1, [], false)
YIELD status, decision, generation
RETURN CASE WHEN status = "OK" AND decision = "STALE_GENERATION"
                 AND generation = 2
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "reprepare without pair edges" '
CALL sync.prepare_model("warehouse", false)
YIELD status, generation, oracle_epoch, pairs, pair_edges,
      materialized_pair_edges, incremental_enabled
RETURN CASE WHEN status = "OK" AND generation = 2 AND oracle_epoch = 2
                 AND pairs = 15 AND pair_edges = 60
                 AND NOT materialized_pair_edges AND NOT incremental_enabled
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "optional pair edges removed" '
MATCH (p:SyncPair {model: "warehouse", oracle_epoch: 2})
WITH count(p) AS pairs
OPTIONAL MATCH (:SyncPair {model: "warehouse"})-[r:PAIR_NEXT|PAIR_PRE]->()
WITH pairs, count(r) AS edges
RETURN CASE WHEN pairs = 15 AND edges = 0
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "cached planning does not require pair relationships" '
CALL sync.plan_sync(
  "warehouse", ["west_bay:east", "east_bay:west"], 64)
YIELD status, outcome, word, generation, oracle_source, oracle_builds
RETURN CASE WHEN status = "OK" AND outcome = "PLAN"
                 AND word = ["to_corridor", "go_west"] AND generation = 2
                 AND oracle_source = "PERSISTED" AND oracle_builds = 0
            THEN "PASS" ELSE "FAIL" END AS result;'

run_query '
MATCH (p:SyncPair {model: "warehouse"}) DETACH DELETE p;
MATCH (m:SyncModel {model: "warehouse"})
SET m.snapshot_token = "integration-missing-pairs";'

expect_failure "cached planning requires pair records" '
CALL sync.plan_sync(
  "warehouse", ["west_bay:east", "east_bay:west"], 64)
YIELD status RETURN status;'

assert_pass "uncached planning ignores missing pair records" '
CALL sync.plan_sync_uncached(
  "warehouse", ["west_bay:east", "east_bay:west"], 64)
YIELD status, outcome, method, word, generation, oracle_source, oracle_builds
RETURN CASE WHEN status = "OK" AND outcome = "PLAN"
                 AND method = "PAIR_MERGE"
                 AND word = ["to_corridor", "go_west"] AND generation = 2
                 AND oracle_source = "RECOMPUTED" AND oracle_builds = 1
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "uncached planning does not recreate pair records" '
MATCH (p:SyncPair {model: "warehouse"})
WITH count(p) AS pairs
RETURN CASE WHEN pairs = 0 THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "prepare incremental oracle" '
CALL sync.prepare_model("warehouse", false, true)
YIELD status, generation, oracle_epoch, pairs, pair_edges,
      materialized_pair_edges, incremental_enabled
RETURN CASE WHEN status = "OK" AND generation = 2 AND oracle_epoch = 3
                 AND pairs = 15 AND pair_edges = 60
                 AND NOT materialized_pair_edges AND incremental_enabled
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "compact incremental oracle stores records only" '
MATCH (p:SyncPair {model: "warehouse", oracle_epoch: 3})
WITH count(p) AS pairs
OPTIONAL MATCH ()-[edge:PAIR_NEXT|PAIR_PRE {
  model: "warehouse", oracle_epoch: 3
}]->()
WITH pairs, count(edge) AS edges
RETURN CASE WHEN pairs = 15 AND edges = 0
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "incremental no-op preserves generation" '
CALL sync.update_cells("warehouse", [{
  state_key: "east_bay:west",
  action_key: "to_wall",
  output_key: "symmetric"
}], 15)
YIELD status, maintenance_mode, generation, oracle_epoch, changed_cells,
      direct_pair_edges, pair_records_touched, pair_records_examined,
      pair_records_written, pair_edges_examined, db_write_batches,
      fallback_rebuild
RETURN CASE WHEN status = "UNCHANGED" AND generation = 2 AND oracle_epoch = 3
                 AND maintenance_mode = "UNCHANGED"
                 AND changed_cells = 0 AND direct_pair_edges = 0
                 AND pair_records_touched = 0 AND pair_records_examined = 0
                 AND pair_records_written = 0 AND pair_edges_examined = 0
                 AND db_write_batches = 0 AND NOT fallback_rebuild
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "incremental local repair" '
CALL sync.update_cells("warehouse", [{
  state_key: "east_bay:west",
  action_key: "to_wall",
  output_key: "east_landmark"
}], 15)
YIELD status, maintenance_mode, generation, oracle_epoch, changed_cells,
      direct_pair_edges, pair_records_touched, pair_records_examined,
      pair_records_written, pair_edges_examined, db_write_batches,
      fallback_rebuild, maintenance_time_us
RETURN CASE WHEN status = "UPDATED" AND generation = 3 AND oracle_epoch = 3
                 AND maintenance_mode = "INCREMENTAL"
                 AND changed_cells = 1 AND direct_pair_edges = 5
                 AND pair_records_touched >= 4 AND pair_records_touched <= 15
                 AND pair_records_examined >= pair_records_touched
                 AND pair_records_written > 0 AND pair_records_written < 15
                 AND pair_edges_examined > 0 AND db_write_batches >= 2
                 AND NOT fallback_rebuild AND maintenance_time_us >= 0
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "compact repair leaves pair relationships absent" '
MATCH (m:SyncModel {model: "warehouse"})
OPTIONAL MATCH ()-[edge:PAIR_NEXT|PAIR_PRE {
  model: "warehouse", oracle_epoch: 3
}]->()
WITH m, count(edge) AS edges
RETURN CASE WHEN NOT m.dirty AND m.generation = 3
                 AND m.prepared_generation = 3 AND m.oracle_epoch = 3
                 AND m.incremental AND NOT m.pair_edges_materialized
                 AND edges = 0
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "cached plan after local repair" '
CALL sync.plan_disambiguate(
  "warehouse", ["west_bay:east", "east_bay:west"], 1, 64)
YIELD status, outcome, word, generation, oracle_source, oracle_builds
RETURN CASE WHEN status = "OK" AND outcome = "PLAN"
                 AND word = ["to_corridor"] AND generation = 3
                 AND oracle_source = "PERSISTED" AND oracle_builds = 0
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "uncached plan agrees after local repair" '
CALL sync.plan_disambiguate_uncached(
  "warehouse", ["west_bay:east", "east_bay:west"], 1, 64)
YIELD status, outcome, word, generation, oracle_source, oracle_builds
RETURN CASE WHEN status = "OK" AND outcome = "PLAN"
                 AND word = ["to_corridor"] AND generation = 3
                 AND oracle_source = "RECOMPUTED" AND oracle_builds = 1
            THEN "PASS" ELSE "FAIL" END AS result;'

expect_failure "invalid incremental batch rolls back" '
CALL sync.update_cells("warehouse", [
  {
    state_key: "east_bay:west",
    action_key: "to_wall",
    output_key: "symmetric"
  },
  {
    state_key: "west_bay:east",
    action_key: "to_wall",
    target_key: "missing"
  }
], 15)
YIELD status RETURN status;'

assert_pass "invalid batch leaves base view and metadata unchanged" '
MATCH (m:SyncModel {model: "warehouse"})
MATCH (:SyncState {model: "warehouse", state_key: "east_bay:west"})
      -[:SYNC_OBS {model: "warehouse", action_key: "to_wall"}]->
      (o:SyncOutput)
RETURN CASE WHEN m.generation = 3 AND m.prepared_generation = 3
                 AND m.oracle_epoch = 3 AND NOT m.dirty
                 AND o.output_key = "east_landmark"
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "bounded repair falls back to rebuild" '
CALL sync.update_cells("warehouse", [{
  state_key: "east_bay:west",
  action_key: "to_wall",
  output_key: "symmetric"
}], 1)
YIELD status, maintenance_mode, generation, oracle_epoch, changed_cells,
      direct_pair_edges, pair_records_touched, pair_records_written,
      fallback_rebuild
RETURN CASE WHEN status = "UPDATED" AND generation = 4 AND oracle_epoch = 3
                 AND maintenance_mode = "FULL_REBUILD"
                 AND changed_cells = 1 AND direct_pair_edges = 5
                 AND pair_records_touched = 15 AND pair_records_written = 15
                 AND fallback_rebuild
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "cached plan after fallback rebuild" '
CALL sync.plan_sync(
  "warehouse", ["west_bay:east", "east_bay:west"], 64)
YIELD status, outcome, word, generation, oracle_source, oracle_builds
RETURN CASE WHEN status = "OK" AND outcome = "PLAN"
                 AND word = ["to_corridor", "go_west"] AND generation = 4
                 AND oracle_source = "PERSISTED" AND oracle_builds = 0
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "uncached plan agrees after fallback rebuild" '
CALL sync.plan_sync_uncached(
  "warehouse", ["west_bay:east", "east_bay:west"], 64)
YIELD status, outcome, word, generation, oracle_source, oracle_builds
RETURN CASE WHEN status = "OK" AND outcome = "PLAN"
                 AND word = ["to_corridor", "go_west"] AND generation = 4
                 AND oracle_source = "RECOMPUTED" AND oracle_builds = 1
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "pre-update plan generation is stale" '
CALL sync.validate_update(
  "warehouse", 3,
  ["west_bay:east", "east_bay:west"],
  ["to_corridor", "go_west"], 1, [], false)
YIELD status, decision, generation
RETURN CASE WHEN status = "OK" AND decision = "STALE_GENERATION"
                 AND generation = 4
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "prepare visual incremental oracle" '
CALL sync.prepare_model("warehouse", true, true)
YIELD status, generation, oracle_epoch, pairs, pair_edges,
      materialized_pair_edges, incremental_enabled
RETURN CASE WHEN status = "OK" AND generation = 4 AND oracle_epoch = 4
                 AND pairs = 15 AND pair_edges = 60
                 AND materialized_pair_edges AND incremental_enabled
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "visual incremental oracle stores both edge directions" '
MATCH (p:SyncPair {model: "warehouse", oracle_epoch: 4})
WITH count(p) AS pairs
OPTIONAL MATCH ()-[next:PAIR_NEXT {model: "warehouse", oracle_epoch: 4}]->()
WITH pairs, count(next) AS next_edges
OPTIONAL MATCH ()-[pre:PAIR_PRE {model: "warehouse", oracle_epoch: 4}]->()
WITH pairs, next_edges, count(pre) AS pre_edges
RETURN CASE WHEN pairs = 15 AND next_edges = 60 AND pre_edges = 60
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "visual incremental repair updates both edge directions" '
CALL sync.update_cells("warehouse", [{
  state_key: "west_bay:east",
  action_key: "to_wall",
  output_key: "west_landmark"
}], 15)
YIELD status, maintenance_mode, generation, oracle_epoch, changed_cells,
      direct_pair_edges, fallback_rebuild
RETURN CASE WHEN status = "UPDATED" AND maintenance_mode = "INCREMENTAL"
                 AND generation = 5 AND oracle_epoch = 4 AND changed_cells = 1
                 AND direct_pair_edges = 5 AND NOT fallback_rebuild
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "visual edge generations advance together" '
MATCH (m:SyncModel {model: "warehouse"})
OPTIONAL MATCH ()-[next:PAIR_NEXT {
  model: "warehouse", oracle_epoch: 4, updated_generation: 5
}]->()
WITH m, count(next) AS updated_next
OPTIONAL MATCH ()-[pre:PAIR_PRE {
  model: "warehouse", oracle_epoch: 4, updated_generation: 5
}]->()
WITH m, updated_next, count(pre) AS updated_pre
RETURN CASE WHEN m.generation = 5 AND m.prepared_generation = 5
                 AND m.pair_edges_materialized AND updated_next = 5
                 AND updated_pre = 5
            THEN "PASS" ELSE "FAIL" END AS result;'

echo "Memgraph integration test passed"
