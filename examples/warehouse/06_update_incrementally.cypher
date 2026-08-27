CALL sync.prepare_model("warehouse", false, true)
YIELD status, generation, oracle_epoch, pairs, pair_edges,
      materialized_pair_edges, incremental_enabled
RETURN status, generation, oracle_epoch, pairs, pair_edges,
       materialized_pair_edges, incremental_enabled;

// When files 00 through 05 were run in order, expected row:
// "OK", 1, 2, 15, 60, true, true

CALL sync.update_cells("warehouse", [{
  state_key: "east_bay:west",
  action_key: "to_wall",
  output_key: "east_landmark"
}], 15)
YIELD status, generation, oracle_epoch, changed_cells, direct_pair_edges,
      fallback_rebuild
RETURN status, generation, oracle_epoch, changed_cells, direct_pair_edges,
       fallback_rebuild;

// Expected row:
// "UPDATED", 2, 2, 1, 5, false
