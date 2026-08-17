// Remove only Sync-KGraph view and auxiliary objects. Application graph nodes
// outside these labels are intentionally left untouched.

MATCH (n)
WHERE n:SyncModel OR n:SyncState OR n:SyncAction OR n:SyncOutput OR n:SyncPair
DETACH DELETE n;

// Drop an adapted dirty-marking trigger separately if one was installed from
// cypher/triggers_mark_dirty.cypher.
