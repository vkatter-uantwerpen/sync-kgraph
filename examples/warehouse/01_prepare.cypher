CALL sync.prepare_model("warehouse", true)
YIELD status, generation, states, actions, outputs, transitions, pairs,
      pair_edges, mergeable_pairs, resolvable_pairs, materialized_pair_edges
RETURN status, generation, states, actions, outputs, transitions, pairs,
       pair_edges, mergeable_pairs, resolvable_pairs, materialized_pair_edges;

// Expected row:
// "OK", 1, 5, 4, 4, 20, 15, 60, 15, 15, true
