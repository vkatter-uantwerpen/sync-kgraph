#!/usr/bin/env sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
  echo "usage: $0 <output.csv> [local | docker <container>]" >&2
  exit 2
fi

output="$1"
case "$output" in
*.csv) update_output="${output%.csv}-updates.csv" ;;
*) update_output="$output-updates.csv" ;;
esac
mode="${2:-local}"
container="${3:-}"
host="${MEMGRAPH_HOST:-127.0.0.1}"
port="${MEMGRAPH_PORT:-7687}"
runs=7

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
  echo "mode must be local or docker" >&2
  exit 2
  ;;
esac

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/sync-kgraph-ablation.XXXXXX")
cleanup() {
  run_query '
MATCH (n)
WHERE n.model IN ["sync_kgraph_ablation", "sync_kgraph_update_ablation"]
  AND (n:SyncModel OR n:SyncState OR n:SyncAction OR n:SyncOutput OR n:SyncPair)
DETACH DELETE n;' >/dev/null 2>&1 || true
  rm -rf "$tmpdir"
}
trap cleanup EXIT HUP INT TERM

run_query '
MATCH (n)
WHERE n.model IN ["sync_kgraph_ablation", "sync_kgraph_update_ablation"]
  AND (n:SyncModel OR n:SyncState OR n:SyncAction OR n:SyncOutput OR n:SyncPair)
DETACH DELETE n;

CREATE (:SyncModel {model: "sync_kgraph_ablation", generation: 1, dirty: true});

UNWIND range(0, 209) AS state
CREATE (:SyncState {
  model: "sync_kgraph_ablation",
  state_key: "s" + toString(state),
  state_id: state
});

UNWIND range(0, 8) AS action
CREATE (:SyncAction {
  model: "sync_kgraph_ablation",
  action_key: "a" + toString(action),
  action_id: action
});

UNWIND range(0, 1) AS output
CREATE (:SyncOutput {
  model: "sync_kgraph_ablation",
  output_key: "o" + toString(output),
  output_id: output
});

UNWIND range(0, 209) AS state
UNWIND range(0, 8) AS action
WITH state, action,
     CASE WHEN action = 0 THEN toInteger(state / 2)
          ELSE (state + action) % 210 END AS target
MATCH (src:SyncState {
  model: "sync_kgraph_ablation", state_key: "s" + toString(state)
})
MATCH (dst:SyncState {
  model: "sync_kgraph_ablation", state_key: "s" + toString(target)
})
CREATE (src)-[:SYNC_TRANS {
  model: "sync_kgraph_ablation", action_key: "a" + toString(action)
}]->(dst);

UNWIND range(0, 209) AS state
UNWIND range(0, 8) AS action
MATCH (src:SyncState {
  model: "sync_kgraph_ablation", state_key: "s" + toString(state)
})
MATCH (output:SyncOutput {
  model: "sync_kgraph_ablation",
  output_key: "o" + toString((state + action) % 2)
})
CREATE (src)-[:SYNC_OBS {
  model: "sync_kgraph_ablation", action_key: "a" + toString(action)
}]->(output);' >/dev/null

prepared=$(run_query '
CALL sync.prepare_model("sync_kgraph_ablation", false)
YIELD status, generation, oracle_epoch, states, actions, pairs, pair_edges,
      materialized_pair_edges, incremental_enabled
RETURN CASE WHEN status = "OK" AND generation = 1 AND oracle_epoch = 1
                 AND states = 210 AND actions = 9
                 AND pairs = 22155 AND pair_edges = 199395
                 AND NOT materialized_pair_edges AND NOT incremental_enabled
            THEN "PASS" ELSE "FAIL" END AS result;')
if ! printf '%s\n' "$prepared" | grep -q 'PASS'; then
  printf '%s\n' "failed to prepare ablation fixture" "$prepared" >&2
  exit 1
fi

measure_call() {
  planner="$1"
  source="$2"
  procedure="$3"
  expected_builds="$4"
  expected_cache="$5"
  if [ "$planner" = "sync" ]; then
    query="
CALL sync.$procedure(
  \"sync_kgraph_ablation\", [\"s208\", \"s209\"], 100000)
YIELD status, outcome, method, word, length, final_state_key,
      final_support_size, generation, oracle_source, oracle_builds,
      oracle_rows_loaded, oracle_load_batches, oracle_cache_hits,
      cache_state, snapshot_record_reads, oracle_time_us,
      snapshot_hydration_time_us, total_compute_time_us
RETURN CASE WHEN status = \"OK\" AND outcome = \"PLAN\"
                 AND method = \"PAIR_MERGE\" AND word = [\"a0\"]
                 AND length = 1 AND final_state_key = \"s104\"
                 AND final_support_size = 1 AND generation = 1
                 AND oracle_source = \"$source\"
                 AND oracle_builds = $expected_builds
                 AND (($expected_builds = 0
                       AND cache_state = \"$expected_cache\"
                       AND oracle_rows_loaded = 0 AND oracle_load_batches = 0
                       AND snapshot_record_reads > 0
                       AND oracle_cache_hits = snapshot_record_reads
                       AND snapshot_hydration_time_us = 0)
                      OR ($expected_builds = 1
                          AND cache_state = \"$expected_cache\"
                          AND oracle_rows_loaded = 0 AND oracle_load_batches = 0
                          AND oracle_cache_hits = 0 AND snapshot_record_reads = 0
                          AND snapshot_hydration_time_us = 0))
                 AND oracle_time_us >= 0
                 AND total_compute_time_us >= oracle_time_us
            THEN \"PASS\" ELSE \"FAIL\" END AS result,
       oracle_time_us, total_compute_time_us,
       oracle_rows_loaded, oracle_load_batches, oracle_cache_hits,
       snapshot_record_reads, snapshot_hydration_time_us;"
  else
    query="
CALL sync.$procedure(
  \"sync_kgraph_ablation\", [\"s208\", \"s209\"], 1, 100000)
YIELD status, outcome, method, word, length, best_support_size,
      worst_support_size, branch_count, homing, generation,
      oracle_source, oracle_builds, oracle_rows_loaded,
      oracle_load_batches, oracle_cache_hits,
      cache_state, snapshot_record_reads, oracle_time_us,
      snapshot_hydration_time_us, total_compute_time_us
RETURN CASE WHEN status = \"OK\" AND outcome = \"PLAN\"
                 AND method = \"PAIR_RESOLUTION\" AND word = [\"a0\"]
                 AND length = 1 AND best_support_size = 1
                 AND worst_support_size = 1 AND branch_count = 2 AND homing
                 AND generation = 1 AND oracle_source = \"$source\"
                 AND oracle_builds = $expected_builds
                 AND (($expected_builds = 0
                       AND cache_state = \"$expected_cache\"
                       AND oracle_rows_loaded = 0 AND oracle_load_batches = 0
                       AND snapshot_record_reads > 0
                       AND oracle_cache_hits = snapshot_record_reads
                       AND snapshot_hydration_time_us = 0)
                      OR ($expected_builds = 1
                          AND cache_state = \"$expected_cache\"
                          AND oracle_rows_loaded = 0 AND oracle_load_batches = 0
                          AND oracle_cache_hits = 0 AND snapshot_record_reads = 0
                          AND snapshot_hydration_time_us = 0))
                 AND oracle_time_us >= 0
                 AND total_compute_time_us >= oracle_time_us
            THEN \"PASS\" ELSE \"FAIL\" END AS result,
       oracle_time_us, total_compute_time_us,
       oracle_rows_loaded, oracle_load_batches, oracle_cache_hits,
       snapshot_record_reads, snapshot_hydration_time_us;"
  fi
  measured=$(run_query "$query") || {
    echo "ablation call failed: $planner $source" >&2
    exit 1
  }
  parsed=$(printf '%s\n' "$measured" | awk -F, '
    END {
      for (field = 1; field <= 8; ++field) {
        gsub(/"/, "", $field)
      }
      print $1, $2, $3, $4, $5, $6, $7, $8
    }')
  set -- $parsed
  if [ "$#" -ne 8 ] || [ "$1" != "PASS" ]; then
    printf '%s\n' "ablation result mismatch: $planner $source" "$measured" >&2
    exit 1
  fi
  printf '%s %s %s %s %s %s %s\n' "$2" "$3" "$4" "$5" "$6" "$7" "$8"
}

median() {
  sort -n "$1" | awk 'NR == 4 { print; exit }'
}

run_mode() {
  planner="$1"
  source="$2"
  procedure="$3"
  expected_builds="$4"
  expected_cache="$5"
  oracle_file="$tmpdir/$planner-$source-oracle"
  total_file="$tmpdir/$planner-$source-total"
  rows_file="$tmpdir/$planner-$source-rows"
  batches_file="$tmpdir/$planner-$source-batches"
  hits_file="$tmpdir/$planner-$source-hits"
  reads_file="$tmpdir/$planner-$source-reads"
  hydration_file="$tmpdir/$planner-$source-hydration"

  measure_call "$planner" "$source" "$procedure" "$expected_builds" \
    "$expected_cache" >/dev/null
  for run in 1 2 3 4 5 6 7; do
    measured=$(measure_call "$planner" "$source" "$procedure" "$expected_builds" \
      "$expected_cache")
    set -- $measured
    printf '%s\n' "$1" >>"$oracle_file"
    printf '%s\n' "$2" >>"$total_file"
    printf '%s\n' "$3" >>"$rows_file"
    printf '%s\n' "$4" >>"$batches_file"
    printf '%s\n' "$5" >>"$hits_file"
    printf '%s\n' "$6" >>"$reads_file"
    printf '%s\n' "$7" >>"$hydration_file"
  done
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$planner" "$source" "$expected_cache" "$runs" "$expected_builds" \
    "$(median "$oracle_file")" "$(median "$total_file")" \
    "$(median "$rows_file")" "$(median "$batches_file")" \
    "$(median "$hits_file")" "$(median "$reads_file")" \
    "$(median "$hydration_file")" >>"$output"
}

printf '%s\n' \
  'planner,oracle_source,cache_state,runs,oracle_builds_per_call,median_oracle_time_us,median_total_compute_time_us,median_oracle_rows_loaded,median_oracle_load_batches,median_oracle_cache_hits,median_snapshot_record_reads,median_snapshot_hydration_time_us' \
  >"$output"
run_mode sync PERSISTED plan_sync 0 HOT
run_mode sync RECOMPUTED plan_sync_uncached 1 BYPASSED
run_mode homing PERSISTED plan_disambiguate 0 HOT
run_mode homing RECOMPUTED plan_disambiguate_uncached 1 BYPASSED

run_query '
CREATE (:SyncModel {
  model: "sync_kgraph_update_ablation", generation: 1, dirty: true
});

UNWIND range(0, 23) AS state
CREATE (:SyncState {
  model: "sync_kgraph_update_ablation",
  state_key: "s" + toString(state),
  state_id: state
});

UNWIND range(0, 4) AS action
CREATE (:SyncAction {
  model: "sync_kgraph_update_ablation",
  action_key: "a" + toString(action),
  action_id: action
});

UNWIND range(0, 1) AS output
CREATE (:SyncOutput {
  model: "sync_kgraph_update_ablation",
  output_key: "o" + toString(output),
  output_id: output
});

UNWIND range(0, 23) AS state
UNWIND range(0, 4) AS action
WITH state, action,
     CASE WHEN action = 0 THEN toInteger(state / 2)
          ELSE (state + action) % 24 END AS target
MATCH (src:SyncState {
  model: "sync_kgraph_update_ablation", state_key: "s" + toString(state)
})
MATCH (dst:SyncState {
  model: "sync_kgraph_update_ablation", state_key: "s" + toString(target)
})
CREATE (src)-[:SYNC_TRANS {
  model: "sync_kgraph_update_ablation", action_key: "a" + toString(action)
}]->(dst);

UNWIND range(0, 23) AS state
UNWIND range(0, 4) AS action
MATCH (src:SyncState {
  model: "sync_kgraph_update_ablation", state_key: "s" + toString(state)
})
MATCH (output:SyncOutput {
  model: "sync_kgraph_update_ablation",
  output_key: "o" + toString((state + action) % 2)
})
CREATE (src)-[:SYNC_OBS {
  model: "sync_kgraph_update_ablation", action_key: "a" + toString(action)
}]->(output);' >/dev/null

update_prepared=$(run_query '
CALL sync.prepare_model("sync_kgraph_update_ablation", false, true)
YIELD status, generation, oracle_epoch, states, actions, pairs, pair_edges,
      materialized_pair_edges, incremental_enabled
RETURN CASE WHEN status = "OK" AND generation = 1 AND oracle_epoch = 1
                 AND states = 24 AND actions = 5
                 AND pairs = 300 AND pair_edges = 1500
                 AND NOT materialized_pair_edges AND incremental_enabled
            THEN "PASS" ELSE "FAIL" END AS result;')
if ! printf '%s\n' "$update_prepared" | grep -q 'PASS'; then
  printf '%s\n' "failed to prepare update ablation fixture" \
    "$update_prepared" >&2
  exit 1
fi

measure_update() {
  label="$1"
  output_key="$2"
  budget="$3"
  expected_generation="$4"
  expected_mode="$5"
  expected_fallback="$6"
  measured=$(run_query "
CALL sync.update_cells(\"sync_kgraph_update_ablation\", [{
  state_key: \"s0\", action_key: \"a4\", output_key: \"$output_key\"
}], $budget)
YIELD status, maintenance_mode, generation, oracle_epoch, changed_cells,
      direct_pair_edges, pair_records_touched, pair_records_examined,
      pair_records_written, pair_edges_examined, db_write_batches,
      fallback_rebuild, maintenance_time_us
RETURN CASE WHEN status = \"UPDATED\" AND generation = $expected_generation
                 AND oracle_epoch = 1 AND changed_cells = 1
                 AND direct_pair_edges = 24
                 AND maintenance_mode = \"$expected_mode\"
                 AND fallback_rebuild = $expected_fallback
                 AND ((maintenance_mode = \"FULL_REBUILD\"
                       AND pair_records_touched = 300
                       AND pair_records_written = 300)
                      OR (maintenance_mode = \"INCREMENTAL\"
                          AND pair_records_touched >= 23
                          AND pair_records_touched < 300
                          AND pair_records_written > 0
                          AND pair_records_written < 300))
                 AND pair_records_examined >= pair_records_touched
                 AND pair_edges_examined > 0 AND db_write_batches >= 2
            THEN \"PASS\" ELSE \"FAIL\" END AS result,
       maintenance_mode, generation, changed_cells, direct_pair_edges,
       pair_records_touched, pair_records_examined, pair_records_written,
       pair_edges_examined, db_write_batches, fallback_rebuild,
       maintenance_time_us;") || {
    echo "update ablation call failed: $label" >&2
    exit 1
  }
  parsed=$(printf '%s\n' "$measured" | awk -F, '
    END {
      for (field = 1; field <= 12; ++field) {
        gsub(/"/, "", $field)
      }
      print $1, $2, $3, $4, $5, $6, $7, $8, $9, $(10), $(11), $(12)
    }')
  set -- $parsed
  if [ "$#" -ne 12 ] || [ "$1" != "PASS" ]; then
    printf '%s\n' "update ablation result mismatch: $label" "$measured" >&2
    exit 1
  fi
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$label" "$2" "$3" "$4" "$5" "$6" "$7" "$8" "$9" "${10}" \
    "${11}" "${12}" >>"$update_output"
}

printf '%s\n' \
  'mode,maintenance_mode,generation,changed_cells,direct_pair_edges,pair_records_touched,pair_records_examined,pair_records_written,pair_edges_examined,db_write_batches,fallback_rebuild,maintenance_time_us' \
  >"$update_output"
measure_update local_repair o1 300 2 INCREMENTAL false
measure_update forced_full_rebuild o0 -1 3 FULL_REBUILD false

post_update_plans=$(run_query '
CALL sync.plan_sync(
  "sync_kgraph_update_ablation", ["s22", "s23"], 100000)
YIELD status, word, generation
WITH status AS cached_status, word AS cached_word, generation AS cached_generation
CALL sync.plan_sync_uncached(
  "sync_kgraph_update_ablation", ["s22", "s23"], 100000)
YIELD status, word, generation
RETURN CASE WHEN cached_status = "OK" AND status = "OK"
                 AND cached_word = ["a0"] AND word = cached_word
                 AND cached_generation = 3 AND generation = cached_generation
            THEN "PASS" ELSE "FAIL" END AS result;')
if ! printf '%s\n' "$post_update_plans" | grep -q 'PASS'; then
  printf '%s\n' "cached and uncached planners diverged after updates" \
    "$post_update_plans" >&2
  exit 1
fi

graph_state=$(run_query '
MATCH (planner:SyncModel {model: "sync_kgraph_ablation"})
OPTIONAL MATCH (p:SyncPair {model: "sync_kgraph_ablation"})
WITH planner, count(p) AS planner_pairs
OPTIONAL MATCH ()-[r:PAIR_NEXT|PAIR_PRE {model: "sync_kgraph_ablation"}]->()
WITH planner, planner_pairs, count(r) AS planner_edges
MATCH (updated:SyncModel {model: "sync_kgraph_update_ablation"})
OPTIONAL MATCH (u:SyncPair {model: "sync_kgraph_update_ablation"})
WITH planner, planner_pairs, planner_edges, updated, count(u) AS update_pairs
OPTIONAL MATCH ()-[e:PAIR_NEXT|PAIR_PRE {
  model: "sync_kgraph_update_ablation"
}]->()
WITH planner, planner_pairs, planner_edges, updated, update_pairs,
     count(e) AS update_edges
RETURN CASE WHEN NOT planner.dirty AND planner.generation = 1
                 AND planner.prepared_generation = 1
                 AND planner.oracle_epoch = 1 AND NOT planner.incremental
                 AND planner_pairs = 22155 AND planner_edges = 0
                 AND NOT updated.dirty AND updated.generation = 3
                 AND updated.prepared_generation = 3
                 AND updated.oracle_epoch = 1 AND updated.incremental
                 AND NOT updated.pair_edges_materialized
                 AND update_pairs = 300 AND update_edges = 0
            THEN "PASS" ELSE "FAIL" END AS result;')
if ! printf '%s\n' "$graph_state" | grep -q 'PASS'; then
  printf '%s\n' "ablation calls changed the prepared graph" "$graph_state" >&2
  exit 1
fi

echo "Ablation work gate passed; planner report: $output"
cat "$output"
echo "Incremental maintenance report: $update_output"
cat "$update_output"
