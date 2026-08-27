#include "snapshot_cache.h"

#include "sync_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct sg_snapshot_cache_entry {
  char *model;
  char *token;
  uint64_t oracle_epoch;
  uint64_t generation;
  size_t bytes;
  sg_pair_snapshot *snapshot;
  struct sg_snapshot_cache_entry *newer;
  struct sg_snapshot_cache_entry *older;
} sg_snapshot_cache_entry;

struct sg_snapshot_cache {
  pthread_mutex_t mutex;
  size_t maximum_bytes;
  size_t bytes;
  sg_snapshot_cache_entry *newest;
  sg_snapshot_cache_entry *oldest;
};

static void sg_cache_entry_free(sg_snapshot_cache_entry *entry) {
  if (entry == NULL) {
    return;
  }
  sg_pair_snapshot_release(entry->snapshot);
  free(entry->model);
  free(entry->token);
  free(entry);
}

static void sg_cache_unlink(sg_snapshot_cache *cache, sg_snapshot_cache_entry *entry) {
  if (entry->newer != NULL) {
    entry->newer->older = entry->older;
  } else {
    cache->newest = entry->older;
  }
  if (entry->older != NULL) {
    entry->older->newer = entry->newer;
  } else {
    cache->oldest = entry->newer;
  }
  entry->newer = NULL;
  entry->older = NULL;
}

static void sg_cache_link_newest(sg_snapshot_cache *cache, sg_snapshot_cache_entry *entry) {
  entry->newer = NULL;
  entry->older = cache->newest;
  if (cache->newest != NULL) {
    cache->newest->newer = entry;
  } else {
    cache->oldest = entry;
  }
  cache->newest = entry;
}

static sg_snapshot_cache_entry *sg_cache_pop_oldest(sg_snapshot_cache *cache) {
  sg_snapshot_cache_entry *entry = cache->oldest;
  if (entry == NULL) {
    return NULL;
  }
  cache->oldest = entry->newer;
  if (cache->oldest != NULL) {
    cache->oldest->older = NULL;
  } else {
    cache->newest = NULL;
  }
  entry->newer = NULL;
  entry->older = NULL;
  return entry;
}

static bool sg_cache_key_equal(const sg_snapshot_cache_entry *entry, const char *model,
                               uint64_t oracle_epoch, uint64_t generation, const char *token) {
  return entry->oracle_epoch == oracle_epoch && entry->generation == generation &&
         strcmp(entry->model, model) == 0 && strcmp(entry->token, token) == 0;
}

sg_status sg_snapshot_cache_create(size_t maximum_bytes, sg_snapshot_cache **cache) {
  if (cache == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *cache = NULL;
  sg_snapshot_cache *created = calloc(1U, sizeof(*created));
  if (created == NULL) {
    return SG_ERR_ALLOC;
  }
  if (pthread_mutex_init(&created->mutex, NULL) != 0) {
    free(created);
    return SG_ERR_ALLOC;
  }
  created->maximum_bytes = maximum_bytes;
  *cache = created;
  return SG_OK;
}

void sg_snapshot_cache_free(sg_snapshot_cache *cache) {
  if (cache == NULL) {
    return;
  }
  (void)pthread_mutex_lock(&cache->mutex);
  sg_snapshot_cache_entry *entry = cache->newest;
  cache->newest = NULL;
  cache->oldest = NULL;
  cache->bytes = 0U;
  (void)pthread_mutex_unlock(&cache->mutex);
  while (entry != NULL) {
    sg_snapshot_cache_entry *older = entry->older;
    sg_cache_entry_free(entry);
    entry = older;
  }
  (void)pthread_mutex_destroy(&cache->mutex);
  free(cache);
}

sg_pair_snapshot *sg_snapshot_cache_lookup(sg_snapshot_cache *cache, const char *model,
                                           uint64_t oracle_epoch, uint64_t generation,
                                           const char *token) {
  if (cache == NULL || model == NULL || token == NULL) {
    return NULL;
  }
  (void)pthread_mutex_lock(&cache->mutex);
  sg_snapshot_cache_entry *entry = cache->newest;
  while (entry != NULL && !sg_cache_key_equal(entry, model, oracle_epoch, generation, token)) {
    entry = entry->older;
  }
  sg_pair_snapshot *snapshot = NULL;
  if (entry != NULL) {
    if (entry != cache->newest) {
      sg_cache_unlink(cache, entry);
      sg_cache_link_newest(cache, entry);
    }
    snapshot = entry->snapshot;
    sg_pair_snapshot_retain(snapshot);
  }
  (void)pthread_mutex_unlock(&cache->mutex);
  return snapshot;
}

sg_status sg_snapshot_cache_insert(sg_snapshot_cache *cache, const char *model,
                                   uint64_t oracle_epoch, uint64_t generation, const char *token,
                                   sg_pair_snapshot *snapshot, bool *stored) {
  if (cache == NULL || model == NULL || token == NULL || snapshot == NULL || stored == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *stored = false;
  const size_t bytes = sg_pair_snapshot_memory_bytes(snapshot);
  if (cache->maximum_bytes == 0U || bytes > cache->maximum_bytes) {
    return SG_OK;
  }
  sg_snapshot_cache_entry *created = calloc(1U, sizeof(*created));
  if (created == NULL) {
    return SG_ERR_ALLOC;
  }
  created->model = sg_string_duplicate(model);
  created->token = sg_string_duplicate(token);
  if (created->model == NULL || created->token == NULL) {
    sg_cache_entry_free(created);
    return SG_ERR_ALLOC;
  }
  created->oracle_epoch = oracle_epoch;
  created->generation = generation;
  created->bytes = bytes;
  created->snapshot = snapshot;
  sg_pair_snapshot_retain(snapshot);

  (void)pthread_mutex_lock(&cache->mutex);
  sg_snapshot_cache_entry *existing = cache->newest;
  while (existing != NULL &&
         !sg_cache_key_equal(existing, model, oracle_epoch, generation, token)) {
    existing = existing->older;
  }
  if (existing != NULL) {
    sg_cache_unlink(cache, existing);
    cache->bytes -= existing->bytes;
  }
  const size_t remaining_bytes = cache->maximum_bytes - created->bytes;
  while (cache->bytes > remaining_bytes && cache->oldest != NULL) {
    sg_snapshot_cache_entry *evicted = sg_cache_pop_oldest(cache);
    if (evicted == NULL) {
      break;
    }
    cache->bytes -= evicted->bytes;
    sg_cache_entry_free(evicted);
  }
  sg_cache_link_newest(cache, created);
  cache->bytes += created->bytes;
  *stored = true;
  (void)pthread_mutex_unlock(&cache->mutex);
  sg_cache_entry_free(existing);
  return SG_OK;
}

size_t sg_snapshot_cache_bytes(sg_snapshot_cache *cache) {
  if (cache == NULL) {
    return 0U;
  }
  (void)pthread_mutex_lock(&cache->mutex);
  const size_t bytes = cache->bytes;
  (void)pthread_mutex_unlock(&cache->mutex);
  return bytes;
}
