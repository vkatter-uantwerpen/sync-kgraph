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
RETURN CASE WHEN procedures = 6 THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "prepare model" '
CALL sync.prepare_model("warehouse", true)
YIELD status, generation, states, actions, outputs, transitions, pairs,
      pair_edges, mergeable_pairs, resolvable_pairs, materialized_pair_edges
RETURN CASE WHEN status = "OK" AND generation = 1 AND states = 5
                 AND actions = 4 AND outputs = 4 AND transitions = 20
                 AND pairs = 15 AND pair_edges = 60
                 AND mergeable_pairs = 15 AND resolvable_pairs = 15
                 AND materialized_pair_edges
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "materialized pair records" '
MATCH (p:SyncPair {model: "warehouse", generation: 1})
WITH count(p) AS pairs
OPTIONAL MATCH (:SyncPair {model: "warehouse", generation: 1})
               -[n:PAIR_NEXT {model: "warehouse", generation: 1}]->
               (:SyncPair {model: "warehouse", generation: 1})
WITH pairs, count(n) AS next_edges
OPTIONAL MATCH (:SyncPair {model: "warehouse", generation: 1})
               -[p:PAIR_PRE {model: "warehouse", generation: 1}]->
               (:SyncPair {model: "warehouse", generation: 1})
WITH pairs, next_edges, count(p) AS pre_edges
RETURN CASE WHEN pairs = 15 AND next_edges = 60 AND pre_edges = 60
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "synchronization plan" '
CALL sync.plan_sync(
  "warehouse", ["west_bay:east", "east_bay:west"], 64)
YIELD status, outcome, method, word, length, final_state_key,
      final_support_size, generation
RETURN CASE WHEN status = "OK" AND outcome = "PLAN"
                 AND method = "PAIR_MERGE"
                 AND word = ["to_corridor", "go_west"] AND length = 2
                 AND final_state_key = "dock:north"
                 AND final_support_size = 1 AND generation = 1
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "disambiguation plan" '
CALL sync.plan_disambiguate(
  "warehouse", ["west_bay:east", "east_bay:west"], 1, 64)
YIELD status, outcome, method, word, length, best_support_size,
      worst_support_size, branch_count, homing, generation
RETURN CASE WHEN status = "OK" AND outcome = "PLAN"
                 AND method = "PAIR_RESOLUTION" AND word = ["to_corridor"]
                 AND length = 1 AND best_support_size = 1
                 AND worst_support_size = 1 AND branch_count = 2
                 AND homing AND generation = 1
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
YIELD status, generation, pairs, pair_edges, materialized_pair_edges
RETURN CASE WHEN status = "OK" AND generation = 2 AND pairs = 15
                 AND pair_edges = 60 AND NOT materialized_pair_edges
            THEN "PASS" ELSE "FAIL" END AS result;'

assert_pass "optional pair edges removed" '
MATCH (p:SyncPair {model: "warehouse", generation: 2})
WITH count(p) AS pairs
OPTIONAL MATCH (:SyncPair {model: "warehouse"})-[r:PAIR_NEXT|PAIR_PRE]->()
WITH pairs, count(r) AS edges
RETURN CASE WHEN pairs = 15 AND edges = 0
            THEN "PASS" ELSE "FAIL" END AS result;'

echo "Memgraph integration test passed"
