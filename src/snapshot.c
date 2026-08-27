#include "snapshot.h"

#include "sync_internal.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SG_SNAPSHOT_RECORD_CHUNK 1024U

typedef struct {
  atomic_size_t references;
  size_t count;
  sg_pair_record records[];
} sg_record_chunk;

typedef struct {
  atomic_size_t references;
  size_t state_count;
  size_t pair_count;
  size_t *first;
  size_t *second;
} sg_pair_topology;

typedef struct {
  sg_pair_arc *items;
  size_t count;
  size_t capacity;
} sg_arc_vector;

struct sg_pair_snapshot {
  atomic_size_t references;
  sg_automaton *automaton;
  sg_pair_topology *topology;
  sg_record_chunk **chunks;
  size_t chunk_count;
  size_t reverse_words;
  uint64_t *reverse_columns;
  bool *changed;
  size_t *changed_pairs;
  size_t changed_count;
  size_t changed_capacity;
};

static bool sg_snapshot_pair_count(size_t states, size_t *pairs) {
  size_t product = 0U;
  if (states == SIZE_MAX || !sg_size_multiply(states, states + 1U, &product)) {
    return false;
  }
  *pairs = product / 2U;
  return true;
}

static sg_record_chunk *sg_record_chunk_create(size_t count) {
  size_t record_bytes = 0U;
  if (!sg_size_multiply(count, sizeof(sg_pair_record), &record_bytes) ||
      record_bytes > SIZE_MAX - sizeof(sg_record_chunk)) {
    return NULL;
  }
  sg_record_chunk *chunk = calloc(1U, sizeof(*chunk) + record_bytes);
  if (chunk != NULL) {
    atomic_init(&chunk->references, 1U);
    chunk->count = count;
  }
  return chunk;
}

static void sg_record_chunk_retain(sg_record_chunk *chunk) {
  (void)atomic_fetch_add_explicit(&chunk->references, 1U, memory_order_relaxed);
}

static void sg_record_chunk_release(sg_record_chunk *chunk) {
  if (chunk != NULL &&
      atomic_fetch_sub_explicit(&chunk->references, 1U, memory_order_acq_rel) == 1U) {
    free(chunk);
  }
}

static sg_status sg_pair_topology_create(size_t state_count, sg_pair_topology **topology) {
  size_t pair_count = 0U;
  if (!sg_snapshot_pair_count(state_count, &pair_count)) {
    return SG_ERR_ALLOC;
  }
  sg_pair_topology *created = calloc(1U, sizeof(*created));
  if (created == NULL) {
    return SG_ERR_ALLOC;
  }
  atomic_init(&created->references, 1U);
  created->state_count = state_count;
  created->pair_count = pair_count;
  created->first = calloc(pair_count, sizeof(*created->first));
  created->second = calloc(pair_count, sizeof(*created->second));
  if (created->first == NULL || created->second == NULL) {
    free(created->first);
    free(created->second);
    free(created);
    return SG_ERR_ALLOC;
  }
  size_t pair = 0U;
  for (size_t first = 0U; first < state_count; ++first) {
    for (size_t second = first; second < state_count; ++second) {
      created->first[pair] = first;
      created->second[pair] = second;
      ++pair;
    }
  }
  *topology = created;
  return SG_OK;
}

static void sg_pair_topology_retain(sg_pair_topology *topology) {
  (void)atomic_fetch_add_explicit(&topology->references, 1U, memory_order_relaxed);
}

static void sg_pair_topology_release(sg_pair_topology *topology) {
  if (topology != NULL &&
      atomic_fetch_sub_explicit(&topology->references, 1U, memory_order_acq_rel) == 1U) {
    free(topology->first);
    free(topology->second);
    free(topology);
  }
}

static bool sg_reverse_column_size(const sg_automaton *automaton, size_t *words,
                                   size_t *word_count) {
  const size_t states = sg_automaton_state_count(automaton);
  *words = (states / 64U) + (states % 64U != 0U ? 1U : 0U);
  size_t columns = 0U;
  return sg_size_multiply(sg_automaton_action_count(automaton), states, &columns) &&
         sg_size_multiply(columns, *words, word_count);
}

static size_t sg_reverse_offset(const sg_pair_snapshot *snapshot, size_t action, size_t target,
                                size_t state) {
  const size_t states = sg_automaton_state_count(snapshot->automaton);
  return (((action * states) + target) * snapshot->reverse_words) + (state / 64U);
}

static bool sg_reverse_has(const sg_pair_snapshot *snapshot, size_t action, size_t target,
                           size_t state) {
  const size_t offset = sg_reverse_offset(snapshot, action, target, state);
  return (snapshot->reverse_columns[offset] & (UINT64_C(1) << (state % 64U))) != 0U;
}

static void sg_reverse_set(sg_pair_snapshot *snapshot, size_t action, size_t target, size_t state,
                           bool present) {
  const size_t offset = sg_reverse_offset(snapshot, action, target, state);
  const uint64_t bit = UINT64_C(1) << (state % 64U);
  if (present) {
    snapshot->reverse_columns[offset] |= bit;
  } else {
    snapshot->reverse_columns[offset] &= ~bit;
  }
}

static sg_status sg_reverse_columns_build(sg_pair_snapshot *snapshot) {
  size_t word_count = 0U;
  if (!sg_reverse_column_size(snapshot->automaton, &snapshot->reverse_words, &word_count)) {
    return SG_ERR_ALLOC;
  }
  snapshot->reverse_columns =
      calloc(word_count == 0U ? 1U : word_count, sizeof(*snapshot->reverse_columns));
  if (snapshot->reverse_columns == NULL) {
    return SG_ERR_ALLOC;
  }
  const size_t states = sg_automaton_state_count(snapshot->automaton);
  const size_t actions = sg_automaton_action_count(snapshot->automaton);
  for (size_t state = 0U; state < states; ++state) {
    for (size_t action = 0U; action < actions; ++action) {
      const size_t target = sg_automaton_transition(snapshot->automaton, state, action);
      if (target >= states) {
        return SG_ERR_INVALID_MODEL;
      }
      sg_reverse_set(snapshot, action, target, state, true);
    }
  }
  return SG_OK;
}

static sg_status sg_snapshot_allocate(const sg_automaton *automaton, sg_pair_snapshot **snapshot) {
  sg_pair_snapshot *created = calloc(1U, sizeof(*created));
  if (created == NULL) {
    return SG_ERR_ALLOC;
  }
  atomic_init(&created->references, 1U);
  sg_status status = sg_automaton_clone_generation(automaton, sg_automaton_generation(automaton),
                                                   &created->automaton);
  if (status == SG_OK) {
    status = sg_pair_topology_create(sg_automaton_state_count(automaton), &created->topology);
  }
  if (status == SG_OK) {
    const size_t pair_count = created->topology->pair_count;
    created->chunk_count = (pair_count / SG_SNAPSHOT_RECORD_CHUNK) +
                           (pair_count % SG_SNAPSHOT_RECORD_CHUNK != 0U ? 1U : 0U);
    created->chunks = calloc(created->chunk_count, sizeof(*created->chunks));
    if (created->chunks == NULL) {
      status = SG_ERR_ALLOC;
    }
  }
  for (size_t chunk = 0U; status == SG_OK && chunk < created->chunk_count; ++chunk) {
    const size_t first = chunk * SG_SNAPSHOT_RECORD_CHUNK;
    const size_t remaining = created->topology->pair_count - first;
    const size_t count =
        remaining < SG_SNAPSHOT_RECORD_CHUNK ? remaining : SG_SNAPSHOT_RECORD_CHUNK;
    created->chunks[chunk] = sg_record_chunk_create(count);
    if (created->chunks[chunk] == NULL) {
      status = SG_ERR_ALLOC;
    }
  }
  if (status == SG_OK) {
    status = sg_reverse_columns_build(created);
  }
  if (status != SG_OK) {
    sg_pair_snapshot_release(created);
    return status;
  }
  *snapshot = created;
  return SG_OK;
}

static sg_pair_record *sg_snapshot_record_mutable(sg_pair_snapshot *snapshot, size_t pair) {
  if (pair >= snapshot->topology->pair_count) {
    return NULL;
  }
  const size_t chunk_id = pair / SG_SNAPSHOT_RECORD_CHUNK;
  const size_t offset = pair % SG_SNAPSHOT_RECORD_CHUNK;
  sg_record_chunk *chunk = snapshot->chunks[chunk_id];
  if (atomic_load_explicit(&chunk->references, memory_order_acquire) != 1U) {
    sg_record_chunk *copy = sg_record_chunk_create(chunk->count);
    if (copy == NULL) {
      return NULL;
    }
    memcpy(copy->records, chunk->records, chunk->count * sizeof(*copy->records));
    snapshot->chunks[chunk_id] = copy;
    sg_record_chunk_release(chunk);
    chunk = copy;
  }
  return &chunk->records[offset];
}

static const sg_pair_record *sg_snapshot_record_const(const sg_pair_snapshot *snapshot,
                                                      size_t pair) {
  if (snapshot == NULL || pair >= snapshot->topology->pair_count) {
    return NULL;
  }
  const size_t chunk_id = pair / SG_SNAPSHOT_RECORD_CHUNK;
  const size_t offset = pair % SG_SNAPSHOT_RECORD_CHUNK;
  return &snapshot->chunks[chunk_id]->records[offset];
}

static sg_status sg_snapshot_fill_from_oracle(sg_pair_snapshot *snapshot,
                                              const sg_pair_oracle *oracle) {
  if (sg_pair_oracle_pair_count(oracle) != snapshot->topology->pair_count) {
    return SG_ERR_INVALID_MODEL;
  }
  for (size_t pair = 0U; pair < snapshot->topology->pair_count; ++pair) {
    sg_pair_record *record = sg_snapshot_record_mutable(snapshot, pair);
    size_t first = 0U;
    size_t second = 0U;
    if (record == NULL || sg_pair_oracle_record(oracle, pair, record) != SG_OK ||
        sg_pair_oracle_pair_states(oracle, pair, &first, &second) != SG_OK ||
        first != snapshot->topology->first[pair] || second != snapshot->topology->second[pair]) {
      return SG_ERR_INVALID_MODEL;
    }
  }
  return SG_OK;
}

sg_status sg_pair_snapshot_from_oracle(const sg_automaton *automaton, const sg_pair_oracle *oracle,
                                       sg_pair_snapshot **snapshot) {
  if (automaton == NULL || oracle == NULL || snapshot == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *snapshot = NULL;
  sg_pair_snapshot *created = NULL;
  sg_status status = sg_snapshot_allocate(automaton, &created);
  if (status == SG_OK) {
    status = sg_snapshot_fill_from_oracle(created, oracle);
  }
  if (status != SG_OK) {
    sg_pair_snapshot_release(created);
    return status;
  }
  *snapshot = created;
  return SG_OK;
}

sg_status sg_pair_snapshot_build(const sg_automaton *automaton, sg_pair_snapshot **snapshot) {
  if (automaton == NULL || snapshot == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  sg_pair_oracle *oracle = NULL;
  sg_status status = sg_pair_oracle_build(automaton, &oracle);
  if (status == SG_OK) {
    status = sg_pair_snapshot_from_oracle(automaton, oracle, snapshot);
  }
  sg_pair_oracle_free(oracle);
  return status;
}

sg_status sg_pair_snapshot_restore(const sg_automaton *automaton, const sg_pair_record *records,
                                   size_t record_count, sg_pair_snapshot **snapshot) {
  if (automaton == NULL || records == NULL || snapshot == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  sg_pair_oracle *oracle = NULL;
  sg_status status = sg_pair_oracle_restore(automaton, records, record_count, &oracle);
  if (status == SG_OK) {
    status = sg_pair_snapshot_from_oracle(automaton, oracle, snapshot);
  }
  sg_pair_oracle_free(oracle);
  return status;
}

sg_status sg_pair_snapshot_clone(const sg_pair_snapshot *source, uint64_t generation,
                                 sg_pair_snapshot **snapshot) {
  if (source == NULL || snapshot == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *snapshot = NULL;
  sg_pair_snapshot *created = calloc(1U, sizeof(*created));
  if (created == NULL) {
    return SG_ERR_ALLOC;
  }
  atomic_init(&created->references, 1U);
  sg_status status =
      sg_automaton_clone_generation(source->automaton, generation, &created->automaton);
  if (status == SG_OK) {
    created->topology = source->topology;
    sg_pair_topology_retain(created->topology);
    created->chunk_count = source->chunk_count;
    created->chunks = calloc(created->chunk_count, sizeof(*created->chunks));
    if (created->chunks == NULL) {
      status = SG_ERR_ALLOC;
    }
  }
  for (size_t chunk = 0U; status == SG_OK && chunk < created->chunk_count; ++chunk) {
    created->chunks[chunk] = source->chunks[chunk];
    sg_record_chunk_retain(created->chunks[chunk]);
  }
  size_t reverse_word_count = 0U;
  if (status == SG_OK &&
      !sg_reverse_column_size(source->automaton, &created->reverse_words, &reverse_word_count)) {
    status = SG_ERR_ALLOC;
  }
  if (status == SG_OK) {
    created->reverse_columns = malloc(reverse_word_count * sizeof(*created->reverse_columns));
    if (created->reverse_columns == NULL) {
      status = SG_ERR_ALLOC;
    } else {
      memcpy(created->reverse_columns, source->reverse_columns,
             reverse_word_count * sizeof(*created->reverse_columns));
    }
  }
  if (status != SG_OK) {
    sg_pair_snapshot_release(created);
    return status;
  }
  *snapshot = created;
  return SG_OK;
}

void sg_pair_snapshot_retain(sg_pair_snapshot *snapshot) {
  if (snapshot != NULL) {
    (void)atomic_fetch_add_explicit(&snapshot->references, 1U, memory_order_relaxed);
  }
}

void sg_pair_snapshot_release(sg_pair_snapshot *snapshot) {
  if (snapshot == NULL ||
      atomic_fetch_sub_explicit(&snapshot->references, 1U, memory_order_acq_rel) != 1U) {
    return;
  }
  for (size_t chunk = 0U; chunk < snapshot->chunk_count; ++chunk) {
    sg_record_chunk_release(snapshot->chunks[chunk]);
  }
  free(snapshot->chunks);
  free(snapshot->reverse_columns);
  free(snapshot->changed);
  free(snapshot->changed_pairs);
  sg_pair_topology_release(snapshot->topology);
  sg_automaton_free(snapshot->automaton);
  free(snapshot);
}

const sg_automaton *sg_pair_snapshot_automaton(const sg_pair_snapshot *snapshot) {
  return snapshot == NULL ? NULL : snapshot->automaton;
}

size_t sg_pair_snapshot_pair_count(const sg_pair_snapshot *snapshot) {
  return snapshot == NULL ? 0U : snapshot->topology->pair_count;
}

size_t sg_pair_snapshot_memory_bytes(const sg_pair_snapshot *snapshot) {
  if (snapshot == NULL) {
    return 0U;
  }
  const size_t states = sg_automaton_state_count(snapshot->automaton);
  const size_t actions = sg_automaton_action_count(snapshot->automaton);
  size_t bytes = sizeof(*snapshot) + (snapshot->chunk_count * sizeof(*snapshot->chunks)) +
                 sizeof(*snapshot->topology) +
                 (snapshot->topology->pair_count * 2U * sizeof(size_t));
  bytes += states * actions * 2U * sizeof(size_t);
  bytes += actions * states * snapshot->reverse_words * sizeof(uint64_t);
  for (size_t chunk = 0U; chunk < snapshot->chunk_count; ++chunk) {
    bytes += sizeof(sg_record_chunk) + (snapshot->chunks[chunk]->count * sizeof(sg_pair_record));
  }
  return bytes;
}

sg_status sg_pair_snapshot_read(const sg_pair_snapshot *snapshot, const size_t *pair_ids,
                                size_t pair_count, sg_pair_record *records) {
  if (snapshot == NULL || pair_ids == NULL || records == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  for (size_t index = 0U; index < pair_count; ++index) {
    const sg_pair_record *record = sg_snapshot_record_const(snapshot, pair_ids[index]);
    if (record == NULL) {
      return SG_ERR_INVALID_ARGUMENT;
    }
    records[index] = *record;
  }
  return SG_OK;
}

sg_status sg_pair_snapshot_record(const sg_pair_snapshot *snapshot, size_t pair,
                                  sg_pair_record *record) {
  return sg_pair_snapshot_read(snapshot, &pair, 1U, record);
}

sg_status sg_pair_snapshot_set_cell(sg_pair_snapshot *snapshot, size_t state, size_t action,
                                    size_t target, size_t output, bool *changed) {
  if (snapshot == NULL || changed == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  const size_t old_target = sg_automaton_transition(snapshot->automaton, state, action);
  const size_t old_output = sg_automaton_observation(snapshot->automaton, state, action);
  if (old_target == SG_INDEX_NONE || old_output == SG_INDEX_NONE) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *changed = old_target != target || old_output != output;
  if (!*changed) {
    return SG_OK;
  }
  const sg_status status =
      sg_automaton_set_cell(snapshot->automaton, state, action, target, output);
  if (status != SG_OK) {
    return status;
  }
  if (old_target != target) {
    sg_reverse_set(snapshot, action, old_target, state, false);
    sg_reverse_set(snapshot, action, target, state, true);
  }
  return SG_OK;
}

static bool sg_arc_vector_append(sg_arc_vector *vector, sg_pair_arc arc) {
  if (vector->count == vector->capacity) {
    const size_t capacity = vector->capacity == 0U ? 64U : vector->capacity * 2U;
    if (capacity < vector->capacity || capacity > SIZE_MAX / sizeof(*vector->items)) {
      return false;
    }
    sg_pair_arc *items = realloc(vector->items, capacity * sizeof(*items));
    if (items == NULL) {
      return false;
    }
    vector->items = items;
    vector->capacity = capacity;
  }
  vector->items[vector->count] = arc;
  ++vector->count;
  return true;
}

static sg_status sg_snapshot_store_read_records(void *context, const size_t *pair_ids,
                                                size_t pair_count, sg_pair_record *records) {
  return sg_pair_snapshot_read(context, pair_ids, pair_count, records);
}

static sg_status sg_snapshot_store_read_outgoing(void *context, const size_t *source_pairs,
                                                 size_t source_count, sg_pair_arc_batch *batch) {
  const sg_pair_snapshot *snapshot = context;
  const size_t actions = sg_automaton_action_count(snapshot->automaton);
  size_t arc_count = 0U;
  if (!sg_size_multiply(source_count, actions, &arc_count)) {
    return SG_ERR_ALLOC;
  }
  batch->items = calloc(arc_count == 0U ? 1U : arc_count, sizeof(*batch->items));
  if (batch->items == NULL) {
    return SG_ERR_ALLOC;
  }
  batch->count = arc_count;
  size_t position = 0U;
  for (size_t index = 0U; index < source_count; ++index) {
    const size_t pair = source_pairs[index];
    if (pair >= snapshot->topology->pair_count) {
      free(batch->items);
      *batch = (sg_pair_arc_batch){0};
      return SG_ERR_INVALID_ARGUMENT;
    }
    const size_t first = snapshot->topology->first[pair];
    const size_t second = snapshot->topology->second[pair];
    for (size_t action = 0U; action < actions; ++action) {
      const size_t first_next = sg_automaton_transition(snapshot->automaton, first, action);
      const size_t second_next = sg_automaton_transition(snapshot->automaton, second, action);
      batch->items[position] = (sg_pair_arc){
          .source_pair = pair,
          .action = action,
          .target_pair = sg_pair_index(snapshot->topology->state_count, first_next, second_next),
          .outputs_differ = sg_automaton_observation(snapshot->automaton, first, action) !=
                            sg_automaton_observation(snapshot->automaton, second, action),
      };
      ++position;
    }
  }
  return SG_OK;
}

static sg_status sg_append_incoming_arc(const sg_pair_snapshot *snapshot, size_t action,
                                        size_t first, size_t second, size_t target_pair,
                                        sg_arc_vector *arcs) {
  const size_t source_pair = sg_pair_index(snapshot->topology->state_count, first, second);
  const sg_pair_arc arc = {
      .source_pair = source_pair,
      .action = action,
      .target_pair = target_pair,
      .outputs_differ = sg_automaton_observation(snapshot->automaton, first, action) !=
                        sg_automaton_observation(snapshot->automaton, second, action),
  };
  return sg_arc_vector_append(arcs, arc) ? SG_OK : SG_ERR_ALLOC;
}

static sg_status sg_snapshot_store_read_incoming(void *context, const size_t *target_pairs,
                                                 size_t target_count, sg_pair_arc_batch *batch) {
  const sg_pair_snapshot *snapshot = context;
  const size_t states = snapshot->topology->state_count;
  const size_t actions = sg_automaton_action_count(snapshot->automaton);
  sg_arc_vector arcs = {0};
  sg_status status = SG_OK;
  for (size_t target_index = 0U; status == SG_OK && target_index < target_count; ++target_index) {
    const size_t target_pair = target_pairs[target_index];
    if (target_pair >= snapshot->topology->pair_count) {
      status = SG_ERR_INVALID_ARGUMENT;
      break;
    }
    bool duplicate = false;
    for (size_t prior = 0U; prior < target_index; ++prior) {
      if (target_pairs[prior] == target_pair) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    const size_t first_target = snapshot->topology->first[target_pair];
    const size_t second_target = snapshot->topology->second[target_pair];
    for (size_t action = 0U; status == SG_OK && action < actions; ++action) {
      if (first_target == second_target) {
        for (size_t first = 0U; status == SG_OK && first < states; ++first) {
          if (!sg_reverse_has(snapshot, action, first_target, first)) {
            continue;
          }
          for (size_t second = first; status == SG_OK && second < states; ++second) {
            if (sg_reverse_has(snapshot, action, first_target, second)) {
              status = sg_append_incoming_arc(snapshot, action, first, second, target_pair, &arcs);
            }
          }
        }
      } else {
        for (size_t first = 0U; status == SG_OK && first < states; ++first) {
          if (!sg_reverse_has(snapshot, action, first_target, first)) {
            continue;
          }
          for (size_t second = 0U; status == SG_OK && second < states; ++second) {
            if (sg_reverse_has(snapshot, action, second_target, second)) {
              status = sg_append_incoming_arc(snapshot, action, first, second, target_pair, &arcs);
            }
          }
        }
      }
    }
  }
  if (status != SG_OK) {
    free(arcs.items);
    return status;
  }
  batch->items = arcs.items;
  batch->count = arcs.count;
  return SG_OK;
}

static bool sg_snapshot_mark_changed(sg_pair_snapshot *snapshot, size_t pair) {
  if (snapshot->changed == NULL) {
    snapshot->changed = calloc(snapshot->topology->pair_count, sizeof(*snapshot->changed));
    if (snapshot->changed == NULL) {
      return false;
    }
  }
  if (snapshot->changed[pair]) {
    return true;
  }
  if (snapshot->changed_count == snapshot->changed_capacity) {
    const size_t capacity =
        snapshot->changed_capacity == 0U ? 64U : snapshot->changed_capacity * 2U;
    if (capacity < snapshot->changed_capacity || capacity > SIZE_MAX / sizeof(size_t)) {
      return false;
    }
    size_t *pairs = realloc(snapshot->changed_pairs, capacity * sizeof(*pairs));
    if (pairs == NULL) {
      return false;
    }
    snapshot->changed_pairs = pairs;
    snapshot->changed_capacity = capacity;
  }
  snapshot->changed[pair] = true;
  snapshot->changed_pairs[snapshot->changed_count] = pair;
  ++snapshot->changed_count;
  return true;
}

static sg_status sg_snapshot_store_write_records(void *context, const sg_pair_record *records,
                                                 size_t record_count) {
  sg_pair_snapshot *snapshot = context;
  for (size_t index = 0U; index < record_count; ++index) {
    const size_t pair = records[index].pair;
    sg_pair_record *destination = sg_snapshot_record_mutable(snapshot, pair);
    if (destination == NULL || !sg_snapshot_mark_changed(snapshot, pair)) {
      return pair >= snapshot->topology->pair_count ? SG_ERR_INVALID_ARGUMENT : SG_ERR_ALLOC;
    }
    *destination = records[index];
  }
  return SG_OK;
}

sg_status sg_pair_snapshot_repair(sg_pair_snapshot *snapshot, const size_t *seed_pairs,
                                  size_t seed_count, size_t repair_budget,
                                  sg_pair_repair_metrics *metrics) {
  if (snapshot == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  const sg_pair_store store = {
      .context = snapshot,
      .state_count = snapshot->topology->state_count,
      .action_count = sg_automaton_action_count(snapshot->automaton),
      .pair_count = snapshot->topology->pair_count,
      .read_records = sg_snapshot_store_read_records,
      .read_outgoing = sg_snapshot_store_read_outgoing,
      .read_incoming = sg_snapshot_store_read_incoming,
      .write_records = sg_snapshot_store_write_records,
  };
  return sg_pair_store_repair(&store, seed_pairs, seed_count, repair_budget, metrics);
}

size_t sg_pair_snapshot_changed_count(const sg_pair_snapshot *snapshot) {
  return snapshot == NULL ? 0U : snapshot->changed_count;
}

const size_t *sg_pair_snapshot_changed_pairs(const sg_pair_snapshot *snapshot) {
  return snapshot == NULL ? NULL : snapshot->changed_pairs;
}

void sg_pair_snapshot_clear_changes(sg_pair_snapshot *snapshot) {
  if (snapshot == NULL) {
    return;
  }
  free(snapshot->changed);
  free(snapshot->changed_pairs);
  snapshot->changed = NULL;
  snapshot->changed_pairs = NULL;
  snapshot->changed_count = 0U;
  snapshot->changed_capacity = 0U;
}
