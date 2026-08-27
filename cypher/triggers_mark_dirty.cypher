// Trigger template only. A schema-agnostic trigger cannot determine which
// application updates feed a model without dirtying unrelated models or the
// writes performed by sync.prepare_model or sync.update_cells itself.
//
// Adapt the event predicate to the source labels and relationships used by
// your mapping, then call the generation update below for each affected model:
//
// MATCH (m:SyncModel {model: affected_model})
// SET m.dirty = true,
//     m.generation = coalesce(m.generation, 0) + 1;
//
// For explicit invalidation, use:
// CALL sync.mark_dirty(affected_model);
//
// Do not run this invalidation for SYNC_TRANS or SYNC_OBS writes made by
// sync.update_cells. That procedure advances generation and repairs the
// incremental oracle atomically. Use this template only for out-of-band writes.
