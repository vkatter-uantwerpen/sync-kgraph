// Optional indexes for the manually materialized Sync-KGraph view.

CREATE INDEX ON :SyncModel(model);
CREATE INDEX ON :SyncState(model);
CREATE INDEX ON :SyncState(state_key);
CREATE INDEX ON :SyncAction(model);
CREATE INDEX ON :SyncAction(action_key);
CREATE INDEX ON :SyncOutput(model);
CREATE INDEX ON :SyncOutput(output_key);
CREATE INDEX ON :SyncPair(model);
CREATE INDEX ON :SyncPair(pair_id);
CREATE INDEX ON :SyncPair(oracle_epoch);

// Application-owned view contract:
// (:SyncModel {
//   model, generation, dirty, prepared_generation?, oracle_epoch?, incremental?,
//   pair_edges_materialized?, snapshot_token?
// })
// (:SyncState {model, state_key, state_id, semantic_ref?, orientation?})
// (:SyncAction {model, action_key, action_id})
// (:SyncOutput {model, output_key, output_id})
// (:SyncState)-[:SYNC_TRANS {model, action_key}]->(:SyncState)
// (:SyncState)-[:SYNC_OBS {model, action_key}]->(:SyncOutput)
//
// sync.prepare_model materializes epoch-scoped SyncPair records.
// Pair visualization adds PAIR_NEXT and PAIR_PRE. Incremental mode requires
// SyncPair records, not pair relationships, and must use sync.update_cells.
