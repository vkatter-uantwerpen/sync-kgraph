#include "dynamic.h"
#include "sync_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  SG_REPAIR_MERGE = 0,
  SG_REPAIR_RESOLUTION,
} sg_repair_objective;

typedef struct {
  size_t *items;
  size_t count;
  size_t capacity;
} sg_id_vector;

typedef struct {
  size_t pair;
  size_t distance;
} sg_heap_entry;

typedef struct {
  sg_heap_entry *items;
  size_t count;
  size_t capacity;
} sg_min_heap;

typedef struct {
  sg_pair_record current;
  sg_pair_record candidate;
} sg_pair_evaluation;

typedef struct {
  size_t distance;
  size_t action;
  size_t target;
  size_t support_count;
} sg_candidate_state;

typedef struct {
  const sg_pair_store *store;
  sg_pair_repair_metrics *metrics;
  size_t repair_budget;
  bool *diagonal;
  bool *touched;
  size_t *positions;
} sg_repair_context;

typedef struct {
  bool *invalid;
  bool *finalized;
  bool *queued;
  bool *changed;
  size_t *tentative;
  sg_id_vector invalidated;
  sg_id_vector decreases;
  sg_min_heap heap;
} sg_objective_state;

static void sg_metric_add(size_t *metric, size_t amount) {
  *metric = amount > SIZE_MAX - *metric ? SIZE_MAX : *metric + amount;
}

static bool sg_id_vector_reserve(sg_id_vector *vector, size_t capacity) {
  if (capacity <= vector->capacity) {
    return true;
  }
  if (capacity > SIZE_MAX / sizeof(*vector->items)) {
    return false;
  }
  size_t *items = realloc(vector->items, capacity * sizeof(*items));
  if (items == NULL) {
    return false;
  }
  vector->items = items;
  vector->capacity = capacity;
  return true;
}

static bool sg_id_vector_append(sg_id_vector *vector, size_t value) {
  if (vector->count == vector->capacity) {
    const size_t capacity = vector->capacity == 0U ? 16U : vector->capacity * 2U;
    if (capacity < vector->capacity || !sg_id_vector_reserve(vector, capacity)) {
      return false;
    }
  }
  vector->items[vector->count] = value;
  ++vector->count;
  return true;
}

static void sg_id_vector_free(sg_id_vector *vector) {
  free(vector->items);
  *vector = (sg_id_vector){0};
}

static bool sg_heap_reserve(sg_min_heap *heap, size_t capacity) {
  if (capacity <= heap->capacity) {
    return true;
  }
  if (capacity > SIZE_MAX / sizeof(*heap->items)) {
    return false;
  }
  sg_heap_entry *items = realloc(heap->items, capacity * sizeof(*items));
  if (items == NULL) {
    return false;
  }
  heap->items = items;
  heap->capacity = capacity;
  return true;
}

static bool sg_heap_push(sg_min_heap *heap, size_t pair, size_t distance) {
  if (heap->count == heap->capacity) {
    const size_t capacity = heap->capacity == 0U ? 16U : heap->capacity * 2U;
    if (capacity < heap->capacity || !sg_heap_reserve(heap, capacity)) {
      return false;
    }
  }
  size_t position = heap->count;
  ++heap->count;
  while (position != 0U) {
    const size_t parent = (position - 1U) / 2U;
    if (heap->items[parent].distance <= distance) {
      break;
    }
    heap->items[position] = heap->items[parent];
    position = parent;
  }
  heap->items[position] = (sg_heap_entry){.pair = pair, .distance = distance};
  return true;
}

static sg_heap_entry sg_heap_pop(sg_min_heap *heap) {
  const sg_heap_entry result = heap->items[0];
  --heap->count;
  if (heap->count == 0U) {
    return result;
  }
  const sg_heap_entry replacement = heap->items[heap->count];
  size_t position = 0U;
  for (;;) {
    const size_t left = (position * 2U) + 1U;
    if (left >= heap->count) {
      break;
    }
    const size_t right = left + 1U;
    size_t child = left;
    if (right < heap->count && heap->items[right].distance < heap->items[left].distance) {
      child = right;
    }
    if (heap->items[child].distance >= replacement.distance) {
      break;
    }
    heap->items[position] = heap->items[child];
    position = child;
  }
  heap->items[position] = replacement;
  return result;
}

static void sg_heap_free(sg_min_heap *heap) {
  free(heap->items);
  *heap = (sg_min_heap){0};
}

static size_t sg_record_distance(const sg_pair_record *record, sg_repair_objective objective) {
  return objective == SG_REPAIR_MERGE ? record->merge_distance : record->resolution_distance;
}

static bool sg_record_objective_equal(const sg_pair_record *first, const sg_pair_record *second,
                                      sg_repair_objective objective) {
  if (objective == SG_REPAIR_MERGE) {
    return first->mergeable == second->mergeable &&
           first->merge_distance == second->merge_distance &&
           first->merge_action == second->merge_action &&
           first->merge_next_pair == second->merge_next_pair &&
           first->merge_support_count == second->merge_support_count;
  }
  return first->resolvable == second->resolvable &&
         first->resolution_distance == second->resolution_distance &&
         first->resolution_action == second->resolution_action &&
         first->resolution_next_pair == second->resolution_next_pair &&
         first->resolution_support_count == second->resolution_support_count;
}

static void sg_record_make_unreachable(sg_pair_record *record, sg_repair_objective objective) {
  if (objective == SG_REPAIR_MERGE) {
    record->mergeable = false;
    record->merge_distance = SG_INDEX_NONE;
    record->merge_action = SG_INDEX_NONE;
    record->merge_next_pair = SG_INDEX_NONE;
    record->merge_support_count = 0U;
  } else {
    record->resolvable = false;
    record->resolution_distance = SG_INDEX_NONE;
    record->resolution_action = SG_INDEX_NONE;
    record->resolution_next_pair = SG_INDEX_NONE;
    record->resolution_support_count = 0U;
  }
}

static void sg_record_make_root(sg_pair_record *record, sg_repair_objective objective) {
  if (objective == SG_REPAIR_MERGE) {
    record->mergeable = true;
    record->merge_distance = 0U;
    record->merge_action = SG_INDEX_NONE;
    record->merge_next_pair = SG_INDEX_NONE;
    record->merge_support_count = 0U;
  } else {
    record->resolvable = true;
    record->resolution_distance = 0U;
    record->resolution_action = SG_INDEX_NONE;
    record->resolution_next_pair = SG_INDEX_NONE;
    record->resolution_support_count = 0U;
  }
}

static bool sg_candidate_add(size_t action, size_t target, size_t distance, bool terminal,
                             sg_candidate_state *best) {
  if (distance == SG_INDEX_NONE) {
    return true;
  }
  if (distance < best->distance) {
    best->distance = distance;
    best->action = action;
    best->target = terminal ? SG_INDEX_NONE : target;
    best->support_count = 1U;
  } else if (distance == best->distance) {
    if (best->support_count == SIZE_MAX) {
      return false;
    }
    ++best->support_count;
    if (action < best->action) {
      best->action = action;
      best->target = terminal ? SG_INDEX_NONE : target;
    }
  }
  return true;
}

static sg_status sg_evaluate_pairs(sg_repair_context *context, const size_t *pair_ids,
                                   size_t pair_count, sg_repair_objective objective,
                                   sg_pair_evaluation *evaluations) {
  if (pair_count == 0U) {
    return SG_OK;
  }
  sg_pair_record *targets = NULL;
  sg_pair_record *current = calloc(pair_count, sizeof(*current));
  sg_pair_arc_batch arcs = {0};
  bool *actions_seen = NULL;
  if (current == NULL) {
    return SG_ERR_ALLOC;
  }
  sg_status status =
      context->store->read_records(context->store->context, pair_ids, pair_count, current);
  sg_metric_add(&context->metrics->pair_records_examined, pair_count);
  if (status != SG_OK) {
    free(current);
    return status;
  }
  for (size_t index = 0U; index < pair_count; ++index) {
    evaluations[index].current = current[index];
  }
  status = context->store->read_outgoing(context->store->context, pair_ids, pair_count, &arcs);
  sg_metric_add(&context->metrics->pair_edges_examined, arcs.count);
  size_t expected_arcs = 0U;
  if (status == SG_OK &&
      (!sg_size_multiply(pair_count, context->store->action_count, &expected_arcs) ||
       arcs.count != expected_arcs)) {
    status = SG_ERR_INVALID_MODEL;
  }
  if (status == SG_OK) {
    targets = calloc(arcs.count == 0U ? 1U : arcs.count, sizeof(*targets));
    actions_seen = calloc(expected_arcs == 0U ? 1U : expected_arcs, sizeof(*actions_seen));
    if (targets == NULL || actions_seen == NULL) {
      status = SG_ERR_ALLOC;
    }
  }
  size_t *target_ids = NULL;
  if (status == SG_OK) {
    target_ids = calloc(arcs.count == 0U ? 1U : arcs.count, sizeof(*target_ids));
    if (target_ids == NULL) {
      status = SG_ERR_ALLOC;
    }
  }
  for (size_t index = 0U; status == SG_OK && index < pair_count; ++index) {
    if (pair_ids[index] >= context->store->pair_count ||
        evaluations[index].current.pair != pair_ids[index] ||
        context->positions[pair_ids[index]] != SG_INDEX_NONE) {
      status = SG_ERR_INVALID_MODEL;
      break;
    }
    context->positions[pair_ids[index]] = index;
    evaluations[index].candidate = evaluations[index].current;
    if (context->diagonal[pair_ids[index]]) {
      sg_record_make_root(&evaluations[index].candidate, objective);
    } else {
      sg_record_make_unreachable(&evaluations[index].candidate, objective);
    }
  }
  for (size_t index = 0U; status == SG_OK && index < arcs.count; ++index) {
    const sg_pair_arc arc = arcs.items[index];
    if (arc.source_pair >= context->store->pair_count ||
        arc.target_pair >= context->store->pair_count ||
        arc.action >= context->store->action_count ||
        context->positions[arc.source_pair] == SG_INDEX_NONE) {
      status = SG_ERR_INVALID_MODEL;
      break;
    }
    const size_t position = context->positions[arc.source_pair];
    const size_t action_slot = (position * context->store->action_count) + arc.action;
    if (actions_seen[action_slot]) {
      status = SG_ERR_INVALID_MODEL;
      break;
    }
    actions_seen[action_slot] = true;
    target_ids[index] = arc.target_pair;
  }
  if (status == SG_OK) {
    status = context->store->read_records(context->store->context, target_ids, arcs.count, targets);
    sg_metric_add(&context->metrics->pair_records_examined, arcs.count);
  }
  for (size_t index = 0U; status == SG_OK && index < arcs.count; ++index) {
    const sg_pair_arc arc = arcs.items[index];
    const size_t position = context->positions[arc.source_pair];
    sg_pair_record *candidate = &evaluations[position].candidate;
    if (targets[index].pair != arc.target_pair || context->diagonal[arc.source_pair]) {
      if (targets[index].pair != arc.target_pair) {
        status = SG_ERR_INVALID_MODEL;
      }
      continue;
    }
    sg_candidate_state best = {
        .distance = sg_record_distance(candidate, objective),
        .action =
            objective == SG_REPAIR_MERGE ? candidate->merge_action : candidate->resolution_action,
        .target = objective == SG_REPAIR_MERGE ? candidate->merge_next_pair
                                               : candidate->resolution_next_pair,
        .support_count = objective == SG_REPAIR_MERGE ? candidate->merge_support_count
                                                      : candidate->resolution_support_count,
    };
    bool terminal = false;
    size_t distance = SG_INDEX_NONE;
    if (objective == SG_REPAIR_RESOLUTION && arc.outputs_differ) {
      distance = 1U;
      terminal = true;
    } else {
      const size_t target_distance = sg_record_distance(&targets[index], objective);
      if (target_distance != SG_INDEX_NONE) {
        if (target_distance >= SG_INDEX_NONE - 1U) {
          status = SG_ERR_INVALID_MODEL;
          break;
        }
        distance = target_distance + 1U;
      }
    }
    if (!sg_candidate_add(arc.action, arc.target_pair, distance, terminal, &best)) {
      status = SG_ERR_RESOURCE_BOUND;
      break;
    }
    if (objective == SG_REPAIR_MERGE) {
      candidate->mergeable = best.distance != SG_INDEX_NONE;
      candidate->merge_distance = best.distance;
      candidate->merge_action = best.action;
      candidate->merge_next_pair = best.target;
      candidate->merge_support_count = best.support_count;
    } else {
      candidate->resolvable = best.distance != SG_INDEX_NONE;
      candidate->resolution_distance = best.distance;
      candidate->resolution_action = best.action;
      candidate->resolution_next_pair = best.target;
      candidate->resolution_support_count = best.support_count;
    }
  }
  for (size_t index = 0U; index < pair_count; ++index) {
    if (pair_ids[index] < context->store->pair_count) {
      context->positions[pair_ids[index]] = SG_INDEX_NONE;
    }
  }
  free(target_ids);
  free(targets);
  free(current);
  free(actions_seen);
  free(arcs.items);
  return status;
}

static bool sg_mark_touched(sg_repair_context *context, size_t pair) {
  if (!context->touched[pair]) {
    context->touched[pair] = true;
    ++context->metrics->pair_records_touched;
  }
  return context->metrics->pair_records_touched <= context->repair_budget;
}

static sg_status sg_write_records(sg_repair_context *context, const sg_pair_record *records,
                                  size_t record_count) {
  if (record_count == 0U) {
    return SG_OK;
  }
  const sg_status status =
      context->store->write_records(context->store->context, records, record_count);
  if (status == SG_OK) {
    sg_metric_add(&context->metrics->pair_records_written, record_count);
  }
  return status;
}

static sg_status sg_objective_state_init(size_t pair_count, sg_objective_state *state) {
  state->invalid = calloc(pair_count, sizeof(*state->invalid));
  state->finalized = calloc(pair_count, sizeof(*state->finalized));
  state->queued = calloc(pair_count, sizeof(*state->queued));
  state->changed = calloc(pair_count, sizeof(*state->changed));
  state->tentative = malloc(pair_count * sizeof(*state->tentative));
  if (state->invalid == NULL || state->finalized == NULL || state->queued == NULL ||
      state->changed == NULL || state->tentative == NULL) {
    return SG_ERR_ALLOC;
  }
  for (size_t pair = 0U; pair < pair_count; ++pair) {
    state->tentative[pair] = SG_INDEX_NONE;
  }
  return SG_OK;
}

static void sg_objective_state_free(sg_objective_state *state) {
  free(state->invalid);
  free(state->finalized);
  free(state->queued);
  free(state->changed);
  free(state->tentative);
  sg_id_vector_free(&state->invalidated);
  sg_id_vector_free(&state->decreases);
  sg_heap_free(&state->heap);
  *state = (sg_objective_state){0};
}

static sg_status sg_collect_sources(sg_repair_context *context, const size_t *targets,
                                    size_t target_count, const bool *excluded,
                                    sg_id_vector *sources) {
  sg_pair_arc_batch arcs = {0};
  sg_status status =
      context->store->read_incoming(context->store->context, targets, target_count, &arcs);
  if (status == SG_OK) {
    sg_metric_add(&context->metrics->pair_edges_examined, arcs.count);
  }
  if (status != SG_OK) {
    return status;
  }
  for (size_t index = 0U; status == SG_OK && index < arcs.count; ++index) {
    const size_t source = arcs.items[index].source_pair;
    if (source >= context->store->pair_count ||
        arcs.items[index].target_pair >= context->store->pair_count ||
        arcs.items[index].action >= context->store->action_count) {
      status = SG_ERR_INVALID_MODEL;
      break;
    }
    if (!context->diagonal[source] && (excluded == NULL || !excluded[source]) &&
        context->positions[source] == SG_INDEX_NONE) {
      context->positions[source] = sources->count;
      if (!sg_id_vector_append(sources, source)) {
        status = SG_ERR_ALLOC;
        break;
      }
    }
  }
  for (size_t index = 0U; index < sources->count; ++index) {
    context->positions[sources->items[index]] = SG_INDEX_NONE;
  }
  free(arcs.items);
  return status;
}

static sg_status sg_seed_objective_changes(sg_repair_context *context, sg_objective_state *state,
                                           const size_t *seed_pairs, size_t seed_count,
                                           sg_repair_objective objective) {
  sg_pair_evaluation *evaluations =
      calloc(seed_count == 0U ? 1U : seed_count, sizeof(*evaluations));
  sg_pair_record *writes = calloc(seed_count == 0U ? 1U : seed_count, sizeof(*writes));
  if (evaluations == NULL || writes == NULL) {
    free(evaluations);
    free(writes);
    return SG_ERR_ALLOC;
  }
  sg_status status = sg_evaluate_pairs(context, seed_pairs, seed_count, objective, evaluations);
  size_t write_count = 0U;
  for (size_t index = 0U; status == SG_OK && index < seed_count; ++index) {
    const size_t pair = seed_pairs[index];
    if (pair >= context->store->pair_count) {
      status = SG_ERR_INVALID_ARGUMENT;
      break;
    }
    if (context->diagonal[pair]) {
      continue;
    }
    if (!sg_mark_touched(context, pair)) {
      status = SG_ERR_RESOURCE_BOUND;
      break;
    }
    const size_t current_distance = sg_record_distance(&evaluations[index].current, objective);
    const size_t candidate_distance = sg_record_distance(&evaluations[index].candidate, objective);
    if (candidate_distance > current_distance) {
      state->invalid[pair] = true;
      sg_record_make_unreachable(&evaluations[index].candidate, objective);
      if (!sg_id_vector_append(&state->invalidated, pair)) {
        status = SG_ERR_ALLOC;
        break;
      }
      if (objective == SG_REPAIR_MERGE) {
        ++context->metrics->merge_pairs_invalidated;
      } else {
        ++context->metrics->resolution_pairs_invalidated;
      }
    } else if (candidate_distance < current_distance && !state->queued[pair]) {
      state->queued[pair] = true;
      if (!sg_id_vector_append(&state->decreases, pair)) {
        status = SG_ERR_ALLOC;
        break;
      }
    }
    if (!sg_record_objective_equal(&evaluations[index].current, &evaluations[index].candidate,
                                   objective)) {
      writes[write_count] = evaluations[index].candidate;
      ++write_count;
      state->changed[pair] = true;
      if (objective == SG_REPAIR_MERGE) {
        ++context->metrics->merge_pairs_changed;
      } else {
        ++context->metrics->resolution_pairs_changed;
      }
    }
  }
  if (status == SG_OK) {
    status = sg_write_records(context, writes, write_count);
  }
  free(writes);
  free(evaluations);
  return status;
}

static sg_status sg_invalidate_dependents(sg_repair_context *context, sg_objective_state *state,
                                          sg_repair_objective objective) {
  size_t head = 0U;
  while (head < state->invalidated.count) {
    const size_t target_count = state->invalidated.count - head;
    sg_id_vector sources = {0};
    sg_status status = sg_collect_sources(context, &state->invalidated.items[head], target_count,
                                          state->invalid, &sources);
    head = state->invalidated.count;
    if (status != SG_OK) {
      sg_id_vector_free(&sources);
      return status;
    }
    sg_pair_evaluation *evaluations =
        calloc(sources.count == 0U ? 1U : sources.count, sizeof(*evaluations));
    if (evaluations == NULL) {
      sg_id_vector_free(&sources);
      return SG_ERR_ALLOC;
    }
    status = sg_evaluate_pairs(context, sources.items, sources.count, objective, evaluations);
    sg_pair_record *writes = calloc(sources.count == 0U ? 1U : sources.count, sizeof(*writes));
    if (writes == NULL) {
      status = SG_ERR_ALLOC;
    }
    size_t write_count = 0U;
    for (size_t index = 0U; status == SG_OK && index < sources.count; ++index) {
      const size_t pair = sources.items[index];
      if (!sg_mark_touched(context, pair)) {
        status = SG_ERR_RESOURCE_BOUND;
        break;
      }
      const size_t current_distance = sg_record_distance(&evaluations[index].current, objective);
      const size_t candidate_distance =
          sg_record_distance(&evaluations[index].candidate, objective);
      if (candidate_distance > current_distance) {
        state->invalid[pair] = true;
        evaluations[index].candidate = evaluations[index].current;
        sg_record_make_unreachable(&evaluations[index].candidate, objective);
        if (!sg_id_vector_append(&state->invalidated, pair)) {
          status = SG_ERR_ALLOC;
          break;
        }
        if (objective == SG_REPAIR_MERGE) {
          ++context->metrics->merge_pairs_invalidated;
        } else {
          ++context->metrics->resolution_pairs_invalidated;
        }
      } else if (candidate_distance < current_distance && !state->queued[pair]) {
        state->queued[pair] = true;
        if (!sg_id_vector_append(&state->decreases, pair)) {
          status = SG_ERR_ALLOC;
          break;
        }
      }
      if (!sg_record_objective_equal(&evaluations[index].current, &evaluations[index].candidate,
                                     objective)) {
        writes[write_count] = evaluations[index].candidate;
        ++write_count;
        if (!state->changed[pair]) {
          state->changed[pair] = true;
          if (objective == SG_REPAIR_MERGE) {
            ++context->metrics->merge_pairs_changed;
          } else {
            ++context->metrics->resolution_pairs_changed;
          }
        }
      }
    }
    if (status == SG_OK && write_count != 0U) {
      status = sg_write_records(context, writes, write_count);
    }
    free(writes);
    free(evaluations);
    sg_id_vector_free(&sources);
    if (status != SG_OK) {
      return status;
    }
  }
  return SG_OK;
}

static sg_status sg_seed_rebuild_heap(sg_repair_context *context, sg_objective_state *state,
                                      sg_repair_objective objective) {
  sg_pair_evaluation *evaluations =
      calloc(state->invalidated.count == 0U ? 1U : state->invalidated.count, sizeof(*evaluations));
  if (evaluations == NULL) {
    return SG_ERR_ALLOC;
  }
  sg_status status = sg_evaluate_pairs(context, state->invalidated.items, state->invalidated.count,
                                       objective, evaluations);
  for (size_t index = 0U; status == SG_OK && index < state->invalidated.count; ++index) {
    const size_t pair = state->invalidated.items[index];
    const size_t distance = sg_record_distance(&evaluations[index].candidate, objective);
    if (distance != SG_INDEX_NONE) {
      state->tentative[pair] = distance;
      if (!sg_heap_push(&state->heap, pair, distance)) {
        status = SG_ERR_ALLOC;
      }
    }
  }
  free(evaluations);
  return status;
}

static sg_status sg_relax_invalid_predecessors(sg_repair_context *context,
                                               sg_objective_state *state, const size_t *targets,
                                               size_t target_count, size_t target_distance,
                                               sg_repair_objective objective) {
  sg_pair_arc_batch arcs = {0};
  sg_status status =
      context->store->read_incoming(context->store->context, targets, target_count, &arcs);
  if (status == SG_OK) {
    sg_metric_add(&context->metrics->pair_edges_examined, arcs.count);
  }
  for (size_t index = 0U; status == SG_OK && index < arcs.count; ++index) {
    const sg_pair_arc arc = arcs.items[index];
    if (arc.source_pair >= context->store->pair_count ||
        arc.target_pair >= context->store->pair_count ||
        arc.action >= context->store->action_count) {
      status = SG_ERR_INVALID_MODEL;
      break;
    }
    const size_t source = arc.source_pair;
    if (!state->invalid[source] || state->finalized[source] || context->diagonal[source]) {
      continue;
    }
    size_t candidate = SG_INDEX_NONE;
    if (objective == SG_REPAIR_RESOLUTION && arc.outputs_differ) {
      candidate = 1U;
    } else if (target_distance < SG_INDEX_NONE - 1U) {
      candidate = target_distance + 1U;
    }
    if (candidate < state->tentative[source]) {
      state->tentative[source] = candidate;
      if (!sg_heap_push(&state->heap, source, candidate)) {
        status = SG_ERR_ALLOC;
      }
    }
  }
  free(arcs.items);
  return status;
}

static sg_status sg_rebuild_invalidated(sg_repair_context *context, sg_objective_state *state,
                                        sg_repair_objective objective) {
  sg_status status = sg_seed_rebuild_heap(context, state, objective);
  while (status == SG_OK && state->heap.count != 0U) {
    const size_t distance = state->heap.items[0].distance;
    sg_id_vector frontier = {0};
    while (state->heap.count != 0U && state->heap.items[0].distance == distance) {
      const sg_heap_entry entry = sg_heap_pop(&state->heap);
      if (!state->finalized[entry.pair] && state->tentative[entry.pair] == entry.distance &&
          !state->queued[entry.pair]) {
        state->queued[entry.pair] = true;
        if (!sg_id_vector_append(&frontier, entry.pair)) {
          status = SG_ERR_ALLOC;
          break;
        }
      }
    }
    for (size_t index = 0U; index < frontier.count; ++index) {
      state->queued[frontier.items[index]] = false;
    }
    if (status != SG_OK || frontier.count == 0U) {
      sg_id_vector_free(&frontier);
      continue;
    }
    sg_pair_evaluation *evaluations = calloc(frontier.count, sizeof(*evaluations));
    if (evaluations == NULL) {
      sg_id_vector_free(&frontier);
      return SG_ERR_ALLOC;
    }
    status = sg_evaluate_pairs(context, frontier.items, frontier.count, objective, evaluations);
    sg_pair_record *writes = calloc(frontier.count, sizeof(*writes));
    sg_id_vector finalized = {0};
    if (writes == NULL) {
      status = SG_ERR_ALLOC;
    }
    size_t write_count = 0U;
    for (size_t index = 0U; status == SG_OK && index < frontier.count; ++index) {
      const size_t pair = frontier.items[index];
      const size_t candidate_distance =
          sg_record_distance(&evaluations[index].candidate, objective);
      if (candidate_distance == SG_INDEX_NONE) {
        continue;
      }
      if (candidate_distance != distance) {
        state->tentative[pair] = candidate_distance;
        if (!sg_heap_push(&state->heap, pair, candidate_distance)) {
          status = SG_ERR_ALLOC;
        }
        continue;
      }
      state->finalized[pair] = true;
      if (!sg_id_vector_append(&finalized, pair)) {
        status = SG_ERR_ALLOC;
        break;
      }
      if (!sg_record_objective_equal(&evaluations[index].current, &evaluations[index].candidate,
                                     objective)) {
        writes[write_count] = evaluations[index].candidate;
        ++write_count;
        if (!state->changed[pair]) {
          state->changed[pair] = true;
          if (objective == SG_REPAIR_MERGE) {
            ++context->metrics->merge_pairs_changed;
          } else {
            ++context->metrics->resolution_pairs_changed;
          }
        }
      }
      if (!state->queued[pair]) {
        state->queued[pair] = true;
        if (!sg_id_vector_append(&state->decreases, pair)) {
          status = SG_ERR_ALLOC;
        }
      }
    }
    if (status == SG_OK && write_count != 0U) {
      status = sg_write_records(context, writes, write_count);
    }
    if (status == SG_OK && finalized.count != 0U) {
      status = sg_relax_invalid_predecessors(context, state, finalized.items, finalized.count,
                                             distance, objective);
    }
    free(writes);
    free(evaluations);
    sg_id_vector_free(&finalized);
    sg_id_vector_free(&frontier);
  }
  return status;
}

static sg_status sg_propagate_decreases(sg_repair_context *context, sg_objective_state *state,
                                        sg_repair_objective objective) {
  size_t head = 0U;
  while (head < state->decreases.count) {
    const size_t target_count = state->decreases.count - head;
    for (size_t index = head; index < state->decreases.count; ++index) {
      state->queued[state->decreases.items[index]] = false;
    }
    sg_id_vector sources = {0};
    sg_status status =
        sg_collect_sources(context, &state->decreases.items[head], target_count, NULL, &sources);
    head = state->decreases.count;
    if (status != SG_OK) {
      sg_id_vector_free(&sources);
      return status;
    }
    sg_pair_evaluation *evaluations =
        calloc(sources.count == 0U ? 1U : sources.count, sizeof(*evaluations));
    if (evaluations == NULL) {
      sg_id_vector_free(&sources);
      return SG_ERR_ALLOC;
    }
    status = sg_evaluate_pairs(context, sources.items, sources.count, objective, evaluations);
    sg_pair_record *writes = calloc(sources.count == 0U ? 1U : sources.count, sizeof(*writes));
    if (writes == NULL) {
      status = SG_ERR_ALLOC;
    }
    size_t write_count = 0U;
    for (size_t index = 0U; status == SG_OK && index < sources.count; ++index) {
      const size_t pair = sources.items[index];
      if (!sg_mark_touched(context, pair)) {
        status = SG_ERR_RESOURCE_BOUND;
        break;
      }
      const size_t current_distance = sg_record_distance(&evaluations[index].current, objective);
      const size_t candidate_distance =
          sg_record_distance(&evaluations[index].candidate, objective);
      if (candidate_distance > current_distance) {
        status = SG_ERR_INVALID_MODEL;
        break;
      }
      if (!sg_record_objective_equal(&evaluations[index].current, &evaluations[index].candidate,
                                     objective)) {
        writes[write_count] = evaluations[index].candidate;
        ++write_count;
        if (!state->changed[pair]) {
          state->changed[pair] = true;
          if (objective == SG_REPAIR_MERGE) {
            ++context->metrics->merge_pairs_changed;
          } else {
            ++context->metrics->resolution_pairs_changed;
          }
        }
      }
      if (candidate_distance < current_distance && !state->queued[pair]) {
        state->queued[pair] = true;
        if (!sg_id_vector_append(&state->decreases, pair)) {
          status = SG_ERR_ALLOC;
        }
      }
    }
    if (status == SG_OK && write_count != 0U) {
      status = sg_write_records(context, writes, write_count);
    }
    free(writes);
    free(evaluations);
    sg_id_vector_free(&sources);
    if (status != SG_OK) {
      return status;
    }
  }
  return SG_OK;
}

static sg_status sg_repair_one_objective(sg_repair_context *context, const size_t *seed_pairs,
                                         size_t seed_count, sg_repair_objective objective) {
  sg_objective_state state = {0};
  sg_status status = sg_objective_state_init(context->store->pair_count, &state);
  if (status == SG_OK) {
    status = sg_seed_objective_changes(context, &state, seed_pairs, seed_count, objective);
  }
  if (status == SG_OK) {
    status = sg_invalidate_dependents(context, &state, objective);
  }
  if (status == SG_OK) {
    status = sg_rebuild_invalidated(context, &state, objective);
  }
  if (status == SG_OK) {
    status = sg_propagate_decreases(context, &state, objective);
  }
  sg_objective_state_free(&state);
  return status;
}

sg_status sg_pair_store_repair(const sg_pair_store *store, const size_t *seed_pairs,
                               size_t seed_count, size_t repair_budget,
                               sg_pair_repair_metrics *metrics) {
  if (store == NULL || seed_pairs == NULL || seed_count == 0U || repair_budget == 0U ||
      metrics == NULL || store->state_count == 0U || store->action_count == 0U ||
      store->pair_count == 0U || store->read_records == NULL || store->read_outgoing == NULL ||
      store->read_incoming == NULL || store->write_records == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *metrics = (sg_pair_repair_metrics){0};
  bool *diagonal = calloc(store->pair_count, sizeof(*diagonal));
  bool *touched = calloc(store->pair_count, sizeof(*touched));
  size_t *positions = malloc(store->pair_count * sizeof(*positions));
  if (diagonal == NULL || touched == NULL || positions == NULL) {
    free(diagonal);
    free(touched);
    free(positions);
    return SG_ERR_ALLOC;
  }
  for (size_t pair = 0U; pair < store->pair_count; ++pair) {
    positions[pair] = SG_INDEX_NONE;
  }
  for (size_t state = 0U; state < store->state_count; ++state) {
    const size_t pair = sg_pair_index(store->state_count, state, state);
    if (pair >= store->pair_count) {
      free(diagonal);
      free(touched);
      free(positions);
      return SG_ERR_INVALID_ARGUMENT;
    }
    diagonal[pair] = true;
  }
  sg_repair_context context = {
      .store = store,
      .metrics = metrics,
      .repair_budget = repair_budget,
      .diagonal = diagonal,
      .touched = touched,
      .positions = positions,
  };
  sg_status status = sg_repair_one_objective(&context, seed_pairs, seed_count, SG_REPAIR_MERGE);
  if (status == SG_OK) {
    status = sg_repair_one_objective(&context, seed_pairs, seed_count, SG_REPAIR_RESOLUTION);
  }
  free(diagonal);
  free(touched);
  free(positions);
  return status;
}
