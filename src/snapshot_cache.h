#ifndef SYNC_KGRAPH_SNAPSHOT_CACHE_H
#define SYNC_KGRAPH_SNAPSHOT_CACHE_H

#include "snapshot.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sg_snapshot_cache sg_snapshot_cache;

sg_status sg_snapshot_cache_create(size_t maximum_bytes, sg_snapshot_cache **cache);
void sg_snapshot_cache_free(sg_snapshot_cache *cache);
sg_pair_snapshot *sg_snapshot_cache_lookup(sg_snapshot_cache *cache, const char *model,
                                           uint64_t oracle_epoch, uint64_t generation,
                                           const char *token);
sg_status sg_snapshot_cache_insert(sg_snapshot_cache *cache, const char *model,
                                   uint64_t oracle_epoch, uint64_t generation, const char *token,
                                   sg_pair_snapshot *snapshot, bool *stored);
size_t sg_snapshot_cache_bytes(sg_snapshot_cache *cache);

#endif
