#ifndef SYNC_KGRAPH_SNAPSHOT_H
#define SYNC_KGRAPH_SNAPSHOT_H

#include "dynamic.h"
#include "sync_kgraph/sync.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sg_pair_snapshot sg_pair_snapshot;

sg_status sg_pair_snapshot_build(const sg_automaton *automaton, sg_pair_snapshot **snapshot);
sg_status sg_pair_snapshot_from_oracle(const sg_automaton *automaton, const sg_pair_oracle *oracle,
                                       sg_pair_snapshot **snapshot);
sg_status sg_pair_snapshot_restore(const sg_automaton *automaton, const sg_pair_record *records,
                                   size_t record_count, sg_pair_snapshot **snapshot);
sg_status sg_pair_snapshot_clone(const sg_pair_snapshot *source, uint64_t generation,
                                 sg_pair_snapshot **snapshot);
void sg_pair_snapshot_retain(sg_pair_snapshot *snapshot);
void sg_pair_snapshot_release(sg_pair_snapshot *snapshot);

const sg_automaton *sg_pair_snapshot_automaton(const sg_pair_snapshot *snapshot);
size_t sg_pair_snapshot_pair_count(const sg_pair_snapshot *snapshot);
size_t sg_pair_snapshot_memory_bytes(const sg_pair_snapshot *snapshot);
sg_status sg_pair_snapshot_read(const sg_pair_snapshot *snapshot, const size_t *pair_ids,
                                size_t pair_count, sg_pair_record *records);
sg_status sg_pair_snapshot_record(const sg_pair_snapshot *snapshot, size_t pair,
                                  sg_pair_record *record);
sg_status sg_pair_snapshot_set_cell(sg_pair_snapshot *snapshot, size_t state, size_t action,
                                    size_t target, size_t output, bool *changed);
sg_status sg_pair_snapshot_repair(sg_pair_snapshot *snapshot, const size_t *seed_pairs,
                                  size_t seed_count, size_t repair_budget,
                                  sg_pair_repair_metrics *metrics);
size_t sg_pair_snapshot_changed_count(const sg_pair_snapshot *snapshot);
const size_t *sg_pair_snapshot_changed_pairs(const sg_pair_snapshot *snapshot);
void sg_pair_snapshot_clear_changes(sg_pair_snapshot *snapshot);

#endif
