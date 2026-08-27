#ifndef SYNC_KGRAPH_DYNAMIC_H
#define SYNC_KGRAPH_DYNAMIC_H

#include "sync_kgraph/sync.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  size_t source_pair;
  size_t action;
  size_t target_pair;
  bool outputs_differ;
} sg_pair_arc;

typedef struct {
  sg_pair_arc *items;
  size_t count;
} sg_pair_arc_batch;

typedef struct {
  void *context;
  size_t state_count;
  size_t action_count;
  size_t pair_count;
  sg_status (*read_records)(void *context, const size_t *pair_ids, size_t pair_count,
                            sg_pair_record *records);
  sg_status (*read_outgoing)(void *context, const size_t *source_pairs, size_t source_count,
                             sg_pair_arc_batch *arcs);
  sg_status (*read_incoming)(void *context, const size_t *target_pairs, size_t target_count,
                             sg_pair_arc_batch *arcs);
  sg_status (*write_records)(void *context, const sg_pair_record *records, size_t record_count);
} sg_pair_store;

typedef struct {
  size_t pair_records_touched;
  size_t pair_records_examined;
  size_t pair_records_written;
  size_t pair_edges_examined;
  size_t merge_pairs_changed;
  size_t merge_pairs_invalidated;
  size_t resolution_pairs_changed;
  size_t resolution_pairs_invalidated;
} sg_pair_repair_metrics;

sg_status sg_pair_store_repair(const sg_pair_store *store, const size_t *seed_pairs,
                               size_t seed_count, size_t repair_budget,
                               sg_pair_repair_metrics *metrics);

#endif
