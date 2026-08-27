CALL sync.prepare_model("warehouse", true)
YIELD status, generation, oracle_epoch, states, actions, outputs, transitions,
      pairs, pair_edges, mergeable_pairs, resolvable_pairs,
      materialized_pair_edges, incremental_enabled
RETURN status, generation, oracle_epoch, states, actions, outputs, transitions,
       pairs, pair_edges, mergeable_pairs, resolvable_pairs,
       materialized_pair_edges, incremental_enabled;

// Expected row:
// "OK", 1, 1, 5, 4, 4, 20, 15, 60, 15, 15, true, false
