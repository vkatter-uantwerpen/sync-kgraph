#include "sync_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t *offsets;
  size_t *pairs;
  size_t *actions;
} sg_predecessors;

size_t sg_pair_index(size_t state_count, size_t first, size_t second) {
  if (first > second) {
    const size_t temporary = first;
    first = second;
    second = temporary;
  }
  return (first * state_count) - ((first * (first + 1U)) / 2U) + second;
}

static bool sg_pair_count(size_t state_count, size_t *pair_count) {
  if (pair_count == NULL || state_count == SIZE_MAX) {
    return false;
  }
  size_t product = 0U;
  if (!sg_size_multiply(state_count, state_count + 1U, &product)) {
    return false;
  }
  *pair_count = product / 2U;
  return true;
}

static void sg_oracle_arrays_initialize(sg_pair_oracle *oracle) {
  const size_t pairs = oracle->pair_count;
  for (size_t pair = 0U; pair < pairs; ++pair) {
    oracle->merge_distance[pair] = SG_INDEX_NONE;
    oracle->merge_action[pair] = SG_INDEX_NONE;
    oracle->merge_next[pair] = SG_INDEX_NONE;
    oracle->resolution_distance[pair] = SG_INDEX_NONE;
    oracle->resolution_action[pair] = SG_INDEX_NONE;
    oracle->resolution_next[pair] = SG_INDEX_NONE;
  }
}

static sg_status sg_oracle_allocate(const sg_automaton *automaton, sg_pair_oracle **oracle) {
  size_t pair_count = 0U;
  if (!sg_pair_count(automaton->state_count, &pair_count)) {
    return SG_ERR_ALLOC;
  }
  size_t edge_count = 0U;
  if (!sg_size_multiply(pair_count, automaton->action_count, &edge_count)) {
    return SG_ERR_ALLOC;
  }
  sg_pair_oracle *created = calloc(1U, sizeof(*created));
  if (created == NULL) {
    return SG_ERR_ALLOC;
  }
  created->automaton = automaton;
  created->pair_count = pair_count;
  created->first = calloc(pair_count, sizeof(*created->first));
  created->second = calloc(pair_count, sizeof(*created->second));
  created->next = calloc(edge_count, sizeof(*created->next));
  created->outputs_differ = calloc(edge_count, sizeof(*created->outputs_differ));
  created->merge_distance = calloc(pair_count, sizeof(*created->merge_distance));
  created->merge_action = calloc(pair_count, sizeof(*created->merge_action));
  created->merge_next = calloc(pair_count, sizeof(*created->merge_next));
  created->resolution_distance = calloc(pair_count, sizeof(*created->resolution_distance));
  created->resolution_action = calloc(pair_count, sizeof(*created->resolution_action));
  created->resolution_next = calloc(pair_count, sizeof(*created->resolution_next));
  if (created->first == NULL || created->second == NULL || created->next == NULL ||
      created->outputs_differ == NULL || created->merge_distance == NULL ||
      created->merge_action == NULL || created->merge_next == NULL ||
      created->resolution_distance == NULL || created->resolution_action == NULL ||
      created->resolution_next == NULL) {
    sg_pair_oracle_free(created);
    return SG_ERR_ALLOC;
  }
  sg_oracle_arrays_initialize(created);
  *oracle = created;
  return SG_OK;
}

static void sg_oracle_fill_pairs(sg_pair_oracle *oracle) {
  const size_t states = oracle->automaton->state_count;
  const size_t actions = oracle->automaton->action_count;
  size_t pair = 0U;
  for (size_t first = 0U; first < states; ++first) {
    for (size_t second = first; second < states; ++second) {
      oracle->first[pair] = first;
      oracle->second[pair] = second;
      for (size_t action = 0U; action < actions; ++action) {
        const size_t first_next = sg_automaton_transition(oracle->automaton, first, action);
        const size_t second_next = sg_automaton_transition(oracle->automaton, second, action);
        const size_t edge = (pair * actions) + action;
        oracle->next[edge] = sg_pair_index(states, first_next, second_next);
        oracle->outputs_differ[edge] = sg_automaton_observation(oracle->automaton, first, action) !=
                                       sg_automaton_observation(oracle->automaton, second, action);
      }
      ++pair;
    }
  }
}

static void sg_predecessors_free(sg_predecessors *predecessors) {
  if (predecessors == NULL) {
    return;
  }
  free(predecessors->offsets);
  free(predecessors->pairs);
  free(predecessors->actions);
  predecessors->offsets = NULL;
  predecessors->pairs = NULL;
  predecessors->actions = NULL;
}

static sg_status sg_predecessors_build(const sg_pair_oracle *oracle,
                                       sg_predecessors *predecessors) {
  const size_t actions = oracle->automaton->action_count;
  const size_t edge_count = oracle->pair_count * actions;
  size_t *counts = calloc(oracle->pair_count, sizeof(*counts));
  predecessors->offsets = calloc(oracle->pair_count + 1U, sizeof(*predecessors->offsets));
  predecessors->pairs = calloc(edge_count, sizeof(*predecessors->pairs));
  predecessors->actions = calloc(edge_count, sizeof(*predecessors->actions));
  if (counts == NULL || predecessors->offsets == NULL || predecessors->pairs == NULL ||
      predecessors->actions == NULL) {
    free(counts);
    sg_predecessors_free(predecessors);
    return SG_ERR_ALLOC;
  }
  for (size_t edge = 0U; edge < edge_count; ++edge) {
    ++counts[oracle->next[edge]];
  }
  for (size_t pair = 0U; pair < oracle->pair_count; ++pair) {
    predecessors->offsets[pair + 1U] = predecessors->offsets[pair] + counts[pair];
    counts[pair] = predecessors->offsets[pair];
  }
  for (size_t pair = 0U; pair < oracle->pair_count; ++pair) {
    for (size_t action = 0U; action < actions; ++action) {
      const size_t target = oracle->next[(pair * actions) + action];
      const size_t position = counts[target];
      predecessors->pairs[position] = pair;
      predecessors->actions[position] = action;
      ++counts[target];
    }
  }
  free(counts);
  return SG_OK;
}

static sg_status sg_merge_distances(sg_pair_oracle *oracle, const sg_predecessors *predecessors) {
  size_t *queue = calloc(oracle->pair_count, sizeof(*queue));
  if (queue == NULL) {
    return SG_ERR_ALLOC;
  }
  size_t head = 0U;
  size_t tail = 0U;
  for (size_t pair = 0U; pair < oracle->pair_count; ++pair) {
    if (oracle->first[pair] == oracle->second[pair]) {
      oracle->merge_distance[pair] = 0U;
      queue[tail] = pair;
      ++tail;
    }
  }
  while (head < tail) {
    const size_t target = queue[head];
    ++head;
    for (size_t index = predecessors->offsets[target]; index < predecessors->offsets[target + 1U];
         ++index) {
      const size_t source = predecessors->pairs[index];
      if (oracle->merge_distance[source] == SG_INDEX_NONE) {
        oracle->merge_distance[source] = oracle->merge_distance[target] + 1U;
        queue[tail] = source;
        ++tail;
      }
    }
  }
  free(queue);
  return SG_OK;
}

static sg_status sg_resolution_distances(sg_pair_oracle *oracle,
                                         const sg_predecessors *predecessors) {
  const size_t actions = oracle->automaton->action_count;
  size_t *queue = calloc(oracle->pair_count, sizeof(*queue));
  if (queue == NULL) {
    return SG_ERR_ALLOC;
  }
  size_t head = 0U;
  size_t tail = 0U;
  for (size_t pair = 0U; pair < oracle->pair_count; ++pair) {
    if (oracle->first[pair] == oracle->second[pair]) {
      oracle->resolution_distance[pair] = 0U;
      queue[tail] = pair;
      ++tail;
    }
  }
  for (size_t pair = 0U; pair < oracle->pair_count; ++pair) {
    if (oracle->resolution_distance[pair] != SG_INDEX_NONE) {
      continue;
    }
    for (size_t action = 0U; action < actions; ++action) {
      if (oracle->outputs_differ[(pair * actions) + action]) {
        oracle->resolution_distance[pair] = 1U;
        queue[tail] = pair;
        ++tail;
        break;
      }
    }
  }
  while (head < tail) {
    const size_t target = queue[head];
    ++head;
    for (size_t index = predecessors->offsets[target]; index < predecessors->offsets[target + 1U];
         ++index) {
      const size_t source = predecessors->pairs[index];
      const size_t action = predecessors->actions[index];
      if (!oracle->outputs_differ[(source * actions) + action] &&
          oracle->resolution_distance[source] == SG_INDEX_NONE) {
        oracle->resolution_distance[source] = oracle->resolution_distance[target] + 1U;
        queue[tail] = source;
        ++tail;
      }
    }
  }
  free(queue);
  return SG_OK;
}

static void sg_choose_witnesses(sg_pair_oracle *oracle) {
  const size_t actions = oracle->automaton->action_count;
  for (size_t pair = 0U; pair < oracle->pair_count; ++pair) {
    if (oracle->merge_distance[pair] != SG_INDEX_NONE && oracle->merge_distance[pair] != 0U) {
      for (size_t action = 0U; action < actions; ++action) {
        const size_t next = oracle->next[(pair * actions) + action];
        if (oracle->merge_distance[next] != SG_INDEX_NONE &&
            oracle->merge_distance[next] + 1U == oracle->merge_distance[pair]) {
          oracle->merge_action[pair] = action;
          oracle->merge_next[pair] = next;
          break;
        }
      }
    }
    if (oracle->resolution_distance[pair] == SG_INDEX_NONE ||
        oracle->resolution_distance[pair] == 0U) {
      continue;
    }
    for (size_t action = 0U; action < actions; ++action) {
      const size_t edge = (pair * actions) + action;
      if (oracle->resolution_distance[pair] == 1U && oracle->outputs_differ[edge]) {
        oracle->resolution_action[pair] = action;
        oracle->resolution_next[pair] = SG_INDEX_NONE;
        break;
      }
      const size_t next = oracle->next[edge];
      if (!oracle->outputs_differ[edge] && oracle->resolution_distance[next] != SG_INDEX_NONE &&
          oracle->resolution_distance[next] + 1U == oracle->resolution_distance[pair]) {
        oracle->resolution_action[pair] = action;
        oracle->resolution_next[pair] = next;
        break;
      }
    }
  }
}

sg_status sg_pair_oracle_build(const sg_automaton *automaton, sg_pair_oracle **oracle) {
  if (automaton == NULL || oracle == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *oracle = NULL;
  sg_pair_oracle *created = NULL;
  sg_status status = sg_oracle_allocate(automaton, &created);
  if (status != SG_OK) {
    return status;
  }
  sg_oracle_fill_pairs(created);
  sg_predecessors predecessors = {0};
  status = sg_predecessors_build(created, &predecessors);
  if (status == SG_OK) {
    status = sg_merge_distances(created, &predecessors);
  }
  if (status == SG_OK) {
    status = sg_resolution_distances(created, &predecessors);
  }
  sg_predecessors_free(&predecessors);
  if (status != SG_OK) {
    sg_pair_oracle_free(created);
    return status;
  }
  sg_choose_witnesses(created);
  *oracle = created;
  return SG_OK;
}

void sg_pair_oracle_free(sg_pair_oracle *oracle) {
  if (oracle == NULL) {
    return;
  }
  free(oracle->first);
  free(oracle->second);
  free(oracle->next);
  free(oracle->outputs_differ);
  free(oracle->merge_distance);
  free(oracle->merge_action);
  free(oracle->merge_next);
  free(oracle->resolution_distance);
  free(oracle->resolution_action);
  free(oracle->resolution_next);
  free(oracle);
}

size_t sg_pair_oracle_pair_count(const sg_pair_oracle *oracle) {
  return oracle == NULL ? 0U : oracle->pair_count;
}

size_t sg_pair_oracle_pair_edge_count(const sg_pair_oracle *oracle) {
  return oracle == NULL ? 0U : oracle->pair_count * oracle->automaton->action_count;
}

static size_t sg_reachable_pair_count(const sg_pair_oracle *oracle, const size_t *distances) {
  if (oracle == NULL) {
    return 0U;
  }
  size_t count = 0U;
  for (size_t pair = 0U; pair < oracle->pair_count; ++pair) {
    if (distances[pair] != SG_INDEX_NONE) {
      ++count;
    }
  }
  return count;
}

size_t sg_pair_oracle_mergeable_pair_count(const sg_pair_oracle *oracle) {
  return oracle == NULL ? 0U : sg_reachable_pair_count(oracle, oracle->merge_distance);
}

size_t sg_pair_oracle_resolvable_pair_count(const sg_pair_oracle *oracle) {
  return oracle == NULL ? 0U : sg_reachable_pair_count(oracle, oracle->resolution_distance);
}

sg_status sg_pair_oracle_pair_states(const sg_pair_oracle *oracle, size_t pair, size_t *first,
                                     size_t *second) {
  if (oracle == NULL || first == NULL || second == NULL || pair >= oracle->pair_count) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *first = oracle->first[pair];
  *second = oracle->second[pair];
  return SG_OK;
}

sg_status sg_pair_oracle_pair_step(const sg_pair_oracle *oracle, size_t pair, size_t action,
                                   size_t *next_pair, bool *outputs_differ) {
  if (oracle == NULL || next_pair == NULL || outputs_differ == NULL || pair >= oracle->pair_count ||
      action >= oracle->automaton->action_count) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  const size_t edge = (pair * oracle->automaton->action_count) + action;
  *next_pair = oracle->next[edge];
  *outputs_differ = oracle->outputs_differ[edge];
  return SG_OK;
}

sg_status sg_pair_oracle_record(const sg_pair_oracle *oracle, size_t pair, sg_pair_record *record) {
  if (oracle == NULL || record == NULL || pair >= oracle->pair_count) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *record = (sg_pair_record){
      .pair = pair,
      .mergeable = oracle->merge_distance[pair] != SG_INDEX_NONE,
      .merge_distance = oracle->merge_distance[pair],
      .merge_action = oracle->merge_action[pair],
      .merge_next_pair = oracle->merge_next[pair],
      .resolvable = oracle->resolution_distance[pair] != SG_INDEX_NONE,
      .resolution_distance = oracle->resolution_distance[pair],
      .resolution_action = oracle->resolution_action[pair],
      .resolution_next_pair = oracle->resolution_next[pair],
  };
  return SG_OK;
}

static bool sg_merge_record_valid(const sg_pair_oracle *oracle, size_t pair) {
  const size_t distance = oracle->merge_distance[pair];
  if (distance == SG_INDEX_NONE) {
    return oracle->merge_action[pair] == SG_INDEX_NONE && oracle->merge_next[pair] == SG_INDEX_NONE;
  }
  if (distance == 0U) {
    return oracle->first[pair] == oracle->second[pair] &&
           oracle->merge_action[pair] == SG_INDEX_NONE && oracle->merge_next[pair] == SG_INDEX_NONE;
  }
  const size_t action = oracle->merge_action[pair];
  const size_t next = oracle->merge_next[pair];
  return action < oracle->automaton->action_count && next < oracle->pair_count &&
         oracle->next[(pair * oracle->automaton->action_count) + action] == next &&
         oracle->merge_distance[next] != SG_INDEX_NONE &&
         oracle->merge_distance[next] + 1U == distance;
}

static bool sg_resolution_record_valid(const sg_pair_oracle *oracle, size_t pair) {
  const size_t distance = oracle->resolution_distance[pair];
  if (distance == SG_INDEX_NONE) {
    return oracle->resolution_action[pair] == SG_INDEX_NONE &&
           oracle->resolution_next[pair] == SG_INDEX_NONE;
  }
  if (distance == 0U) {
    return oracle->first[pair] == oracle->second[pair] &&
           oracle->resolution_action[pair] == SG_INDEX_NONE &&
           oracle->resolution_next[pair] == SG_INDEX_NONE;
  }
  const size_t action = oracle->resolution_action[pair];
  if (action >= oracle->automaton->action_count) {
    return false;
  }
  const size_t edge = (pair * oracle->automaton->action_count) + action;
  if (oracle->resolution_next[pair] == SG_INDEX_NONE) {
    return distance == 1U && oracle->outputs_differ[edge];
  }
  const size_t next = oracle->resolution_next[pair];
  return next < oracle->pair_count && !oracle->outputs_differ[edge] && oracle->next[edge] == next &&
         oracle->resolution_distance[next] != SG_INDEX_NONE &&
         oracle->resolution_distance[next] + 1U == distance;
}

sg_status sg_pair_oracle_restore(const sg_automaton *automaton, const sg_pair_record *records,
                                 size_t record_count, sg_pair_oracle **oracle) {
  if (automaton == NULL || records == NULL || oracle == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *oracle = NULL;
  sg_pair_oracle *created = NULL;
  sg_status status = sg_oracle_allocate(automaton, &created);
  if (status != SG_OK) {
    return status;
  }
  if (record_count != created->pair_count) {
    sg_pair_oracle_free(created);
    return SG_ERR_INVALID_MODEL;
  }
  sg_oracle_fill_pairs(created);
  bool *seen = calloc(created->pair_count, sizeof(*seen));
  if (seen == NULL) {
    sg_pair_oracle_free(created);
    return SG_ERR_ALLOC;
  }
  for (size_t index = 0U; index < record_count; ++index) {
    const sg_pair_record record = records[index];
    if (record.pair >= created->pair_count || seen[record.pair]) {
      status = SG_ERR_INVALID_MODEL;
      break;
    }
    seen[record.pair] = true;
    created->merge_distance[record.pair] = record.mergeable ? record.merge_distance : SG_INDEX_NONE;
    created->merge_action[record.pair] = record.mergeable ? record.merge_action : SG_INDEX_NONE;
    created->merge_next[record.pair] = record.mergeable ? record.merge_next_pair : SG_INDEX_NONE;
    created->resolution_distance[record.pair] =
        record.resolvable ? record.resolution_distance : SG_INDEX_NONE;
    created->resolution_action[record.pair] =
        record.resolvable ? record.resolution_action : SG_INDEX_NONE;
    created->resolution_next[record.pair] =
        record.resolvable ? record.resolution_next_pair : SG_INDEX_NONE;
  }
  for (size_t pair = 0U; status == SG_OK && pair < created->pair_count; ++pair) {
    if (!seen[pair] || !sg_merge_record_valid(created, pair) ||
        !sg_resolution_record_valid(created, pair)) {
      status = SG_ERR_INVALID_MODEL;
    }
  }
  free(seen);
  if (status != SG_OK) {
    sg_pair_oracle_free(created);
    return status;
  }
  *oracle = created;
  return SG_OK;
}

static sg_status sg_witness_word(const sg_pair_oracle *oracle, size_t first, size_t second,
                                 bool resolution, sg_word *word) {
  if (oracle == NULL || word == NULL || first >= oracle->automaton->state_count ||
      second >= oracle->automaton->state_count) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  sg_status status = sg_word_init(word);
  if (status != SG_OK) {
    return status;
  }
  size_t pair = sg_pair_index(oracle->automaton->state_count, first, second);
  const size_t *distances = resolution ? oracle->resolution_distance : oracle->merge_distance;
  const size_t *actions = resolution ? oracle->resolution_action : oracle->merge_action;
  const size_t *next_pairs = resolution ? oracle->resolution_next : oracle->merge_next;
  if (distances[pair] == SG_INDEX_NONE) {
    sg_word_free(word);
    return SG_ERR_NOT_FOUND;
  }
  while (distances[pair] != 0U) {
    if (actions[pair] == SG_INDEX_NONE) {
      sg_word_free(word);
      return SG_ERR_INVALID_MODEL;
    }
    status = sg_word_append(word, actions[pair]);
    if (status != SG_OK) {
      sg_word_free(word);
      return status;
    }
    if (resolution && next_pairs[pair] == SG_INDEX_NONE) {
      break;
    }
    pair = next_pairs[pair];
    if (pair >= oracle->pair_count) {
      sg_word_free(word);
      return SG_ERR_INVALID_MODEL;
    }
  }
  return SG_OK;
}

sg_status sg_pair_oracle_merge_word(const sg_pair_oracle *oracle, size_t first, size_t second,
                                    sg_word *word) {
  return sg_witness_word(oracle, first, second, false, word);
}

sg_status sg_pair_oracle_resolution_word(const sg_pair_oracle *oracle, size_t first, size_t second,
                                         sg_word *word) {
  return sg_witness_word(oracle, first, second, true, word);
}
