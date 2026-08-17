#include "sync_internal.h"

#include <stdlib.h>
#include <string.h>

#define SG_BITSET_WORD_BITS 64U

typedef struct {
  uint64_t *words;
  size_t word_count;
  size_t state_count;
} sg_bitset;

typedef struct {
  sg_bitset support;
  size_t *trace;
  size_t trace_length;
} sg_trace_branch;

typedef struct {
  sg_trace_branch *branches;
  size_t count;
  size_t capacity;
} sg_trace_partition;

typedef struct {
  sg_bitset *supports;
  size_t count;
  size_t capacity;
} sg_support_partition;

typedef struct {
  sg_support_partition partition;
  size_t parent;
  size_t action;
} sg_search_node;

typedef struct {
  sg_search_node *nodes;
  size_t count;
  size_t capacity;
} sg_search_nodes;

static sg_status sg_bitset_init(sg_bitset *set, size_t state_count) {
  if (set == NULL || state_count == 0U) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  set->state_count = state_count;
  set->word_count = (state_count + (SG_BITSET_WORD_BITS - 1U)) / SG_BITSET_WORD_BITS;
  set->words = calloc(set->word_count, sizeof(*set->words));
  return set->words == NULL ? SG_ERR_ALLOC : SG_OK;
}

static void sg_bitset_free(sg_bitset *set) {
  if (set == NULL) {
    return;
  }
  free(set->words);
  set->words = NULL;
  set->word_count = 0U;
  set->state_count = 0U;
}

static void sg_bitset_clear(sg_bitset *set) {
  memset(set->words, 0, set->word_count * sizeof(*set->words));
}

static void sg_bitset_add(sg_bitset *set, size_t state) {
  set->words[state / SG_BITSET_WORD_BITS] |= UINT64_C(1) << (state % SG_BITSET_WORD_BITS);
}

static bool sg_bitset_has(const sg_bitset *set, size_t state) {
  return (set->words[state / SG_BITSET_WORD_BITS] &
          (UINT64_C(1) << (state % SG_BITSET_WORD_BITS))) != 0U;
}

static size_t sg_popcount(uint64_t value) {
  size_t count = 0U;
  while (value != 0U) {
    value &= value - 1U;
    ++count;
  }
  return count;
}

static size_t sg_bitset_count(const sg_bitset *set) {
  size_t count = 0U;
  for (size_t index = 0U; index < set->word_count; ++index) {
    count += sg_popcount(set->words[index]);
  }
  return count;
}

static bool sg_bitset_equal(const sg_bitset *first, const sg_bitset *second) {
  return first->state_count == second->state_count && first->word_count == second->word_count &&
         memcmp(first->words, second->words, first->word_count * sizeof(*first->words)) == 0;
}

static bool sg_bitset_subset(const sg_bitset *first, const sg_bitset *second) {
  if (first->state_count != second->state_count || first->word_count != second->word_count) {
    return false;
  }
  for (size_t index = 0U; index < first->word_count; ++index) {
    if ((first->words[index] & ~second->words[index]) != 0U) {
      return false;
    }
  }
  return true;
}

static int sg_bitset_compare(const sg_bitset *first, const sg_bitset *second) {
  for (size_t index = first->word_count; index > 0U; --index) {
    const uint64_t first_word = first->words[index - 1U];
    const uint64_t second_word = second->words[index - 1U];
    if (first_word < second_word) {
      return -1;
    }
    if (first_word > second_word) {
      return 1;
    }
  }
  return 0;
}

static sg_status sg_bitset_copy(const sg_bitset *source, sg_bitset *destination) {
  sg_status status = sg_bitset_init(destination, source->state_count);
  if (status == SG_OK) {
    memcpy(destination->words, source->words, source->word_count * sizeof(*source->words));
  }
  return status;
}

static sg_status sg_bitset_from_ids(size_t state_count, const size_t *states, size_t count,
                                    sg_bitset *set) {
  if (states == NULL || count == 0U || set == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  sg_status status = sg_bitset_init(set, state_count);
  if (status != SG_OK) {
    return status;
  }
  for (size_t index = 0U; index < count; ++index) {
    if (states[index] >= state_count) {
      sg_bitset_free(set);
      return SG_ERR_INVALID_ARGUMENT;
    }
    sg_bitset_add(set, states[index]);
  }
  return SG_OK;
}

static void sg_bitset_to_ids(const sg_bitset *set, size_t *states, size_t *count) {
  size_t position = 0U;
  for (size_t state = 0U; state < set->state_count; ++state) {
    if (sg_bitset_has(set, state)) {
      states[position] = state;
      ++position;
    }
  }
  *count = position;
}

static void sg_apply_action_set(const sg_automaton *automaton, const sg_bitset *source,
                                size_t action, sg_bitset *destination) {
  sg_bitset_clear(destination);
  for (size_t state = 0U; state < automaton->state_count; ++state) {
    if (sg_bitset_has(source, state)) {
      sg_bitset_add(destination, sg_automaton_transition(automaton, state, action));
    }
  }
}

static sg_status sg_apply_word_set(const sg_automaton *automaton, const sg_bitset *source,
                                   const sg_word *word, sg_bitset *destination) {
  sg_bitset current = {0};
  sg_bitset next = {0};
  sg_status status = sg_bitset_copy(source, &current);
  if (status == SG_OK) {
    status = sg_bitset_init(&next, automaton->state_count);
  }
  if (status != SG_OK) {
    sg_bitset_free(&current);
    return status;
  }
  for (size_t index = 0U; index < word->length; ++index) {
    if (word->actions[index] >= automaton->action_count) {
      sg_bitset_free(&current);
      sg_bitset_free(&next);
      return SG_ERR_INVALID_ARGUMENT;
    }
    sg_apply_action_set(automaton, &current, word->actions[index], &next);
    sg_bitset temporary = current;
    current = next;
    next = temporary;
  }
  sg_bitset_free(&next);
  *destination = current;
  return SG_OK;
}

sg_status sg_apply_word(const sg_automaton *automaton, const size_t *initial_states,
                        size_t initial_count, const sg_word *word, size_t *output_states,
                        size_t *output_count) {
  if (automaton == NULL || initial_states == NULL || initial_count == 0U || word == NULL ||
      output_states == NULL || output_count == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  sg_bitset initial = {0};
  sg_bitset final = {0};
  sg_status status =
      sg_bitset_from_ids(automaton->state_count, initial_states, initial_count, &initial);
  if (status == SG_OK) {
    status = sg_apply_word_set(automaton, &initial, word, &final);
  }
  if (status == SG_OK) {
    sg_bitset_to_ids(&final, output_states, output_count);
  }
  sg_bitset_free(&initial);
  sg_bitset_free(&final);
  return status;
}

static void sg_plan_result_reset(sg_plan_result *result) {
  result->outcome = SG_OUTCOME_NO_PLAN;
  result->method = SG_METHOD_NONE;
  result->final_state = SG_INDEX_NONE;
  result->final_support_size = 0U;
  result->best_support_size = 0U;
  result->worst_support_size = 0U;
  result->branch_count = 0U;
  result->expansions = 0U;
  result->homing = false;
  result->generation = 0U;
  result->planning_time_us = 0U;
}

static sg_status sg_plan_result_init(const sg_automaton *automaton, sg_plan_result *result) {
  if (result == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  sg_plan_result_reset(result);
  result->generation = automaton->generation;
  return sg_word_init(&result->word);
}

void sg_plan_result_free(sg_plan_result *result) {
  if (result == NULL) {
    return;
  }
  sg_word_free(&result->word);
  sg_plan_result_reset(result);
}

static uint64_t sg_elapsed_us(uint64_t start) {
  const uint64_t end = sg_monotonic_time_us();
  return end >= start ? end - start : 0U;
}

static bool sg_sync_candidate_better(size_t count, size_t length, size_t pair, size_t best_count,
                                     size_t best_length, size_t best_pair) {
  return count < best_count ||
         (count == best_count &&
          (length < best_length || (length == best_length && pair < best_pair)));
}

static sg_status sg_best_merge(const sg_automaton *automaton, const sg_pair_oracle *oracle,
                               const sg_bitset *active, sg_word *best_word,
                               sg_bitset *best_support) {
  size_t best_count = SG_INDEX_NONE;
  size_t best_length = SG_INDEX_NONE;
  size_t best_pair = SG_INDEX_NONE;
  sg_status status = SG_ERR_NOT_FOUND;
  for (size_t first = 0U; first < automaton->state_count; ++first) {
    if (!sg_bitset_has(active, first)) {
      continue;
    }
    for (size_t second = first + 1U; second < automaton->state_count; ++second) {
      if (!sg_bitset_has(active, second)) {
        continue;
      }
      sg_word candidate_word = {0};
      if (sg_pair_oracle_merge_word(oracle, first, second, &candidate_word) != SG_OK) {
        continue;
      }
      sg_bitset candidate_support = {0};
      status = sg_apply_word_set(automaton, active, &candidate_word, &candidate_support);
      if (status != SG_OK) {
        sg_word_free(&candidate_word);
        return status;
      }
      const size_t count = sg_bitset_count(&candidate_support);
      const size_t pair = sg_pair_index(automaton->state_count, first, second);
      if (sg_sync_candidate_better(count, candidate_word.length, pair, best_count, best_length,
                                   best_pair)) {
        sg_word_free(best_word);
        sg_bitset_free(best_support);
        *best_word = candidate_word;
        *best_support = candidate_support;
        best_count = count;
        best_length = candidate_word.length;
        best_pair = pair;
        status = SG_OK;
      } else {
        sg_word_free(&candidate_word);
        sg_bitset_free(&candidate_support);
      }
    }
  }
  return best_pair == SG_INDEX_NONE ? SG_ERR_NOT_FOUND : status;
}

static void sg_trace_partition_free(sg_trace_partition *partition) {
  if (partition == NULL) {
    return;
  }
  for (size_t index = 0U; index < partition->count; ++index) {
    sg_bitset_free(&partition->branches[index].support);
    free(partition->branches[index].trace);
  }
  free(partition->branches);
  partition->branches = NULL;
  partition->count = 0U;
  partition->capacity = 0U;
}

static sg_status sg_trace_partition_add(sg_trace_partition *partition, sg_bitset *support,
                                        const size_t *trace, size_t trace_length,
                                        size_t appended_output) {
  if (partition->count == partition->capacity) {
    const size_t capacity = partition->capacity == 0U ? 8U : partition->capacity * 2U;
    size_t bytes = 0U;
    if (capacity < partition->capacity ||
        !sg_size_multiply(capacity, sizeof(*partition->branches), &bytes)) {
      return SG_ERR_ALLOC;
    }
    sg_trace_branch *branches = realloc(partition->branches, bytes);
    if (branches == NULL) {
      return SG_ERR_ALLOC;
    }
    partition->branches = branches;
    partition->capacity = capacity;
  }
  size_t *new_trace = NULL;
  if (trace_length < SIZE_MAX) {
    new_trace = malloc((trace_length + 1U) * sizeof(*new_trace));
  }
  if (new_trace == NULL) {
    return SG_ERR_ALLOC;
  }
  if (trace_length != 0U) {
    memcpy(new_trace, trace, trace_length * sizeof(*trace));
  }
  new_trace[trace_length] = appended_output;
  partition->branches[partition->count] = (sg_trace_branch){
      .support = *support,
      .trace = new_trace,
      .trace_length = trace_length + 1U,
  };
  support->words = NULL;
  ++partition->count;
  return SG_OK;
}

static sg_status sg_trace_partition_init(const sg_bitset *initial, sg_trace_partition *partition) {
  partition->branches = calloc(1U, sizeof(*partition->branches));
  if (partition->branches == NULL) {
    return SG_ERR_ALLOC;
  }
  partition->capacity = 1U;
  partition->count = 1U;
  const sg_status status = sg_bitset_copy(initial, &partition->branches[0].support);
  if (status != SG_OK) {
    sg_trace_partition_free(partition);
  }
  return status;
}

static sg_status sg_trace_partition_apply_action(const sg_automaton *automaton,
                                                 const sg_trace_partition *source, size_t action,
                                                 sg_trace_partition *destination) {
  for (size_t branch = 0U; branch < source->count; ++branch) {
    sg_bitset *groups = calloc(automaton->output_count, sizeof(*groups));
    bool *used = calloc(automaton->output_count, sizeof(*used));
    if (groups == NULL || used == NULL) {
      free(groups);
      free(used);
      sg_trace_partition_free(destination);
      return SG_ERR_ALLOC;
    }
    sg_status status = SG_OK;
    for (size_t state = 0U; state < automaton->state_count; ++state) {
      if (!sg_bitset_has(&source->branches[branch].support, state)) {
        continue;
      }
      const size_t output = sg_automaton_observation(automaton, state, action);
      if (!used[output]) {
        status = sg_bitset_init(&groups[output], automaton->state_count);
        if (status != SG_OK) {
          break;
        }
        used[output] = true;
      }
      if (groups[output].words == NULL) {
        status = SG_ERR_INVALID_MODEL;
        break;
      }
      sg_bitset_add(&groups[output], sg_automaton_transition(automaton, state, action));
    }
    for (size_t output = 0U; status == SG_OK && output < automaton->output_count; ++output) {
      if (used[output]) {
        status =
            sg_trace_partition_add(destination, &groups[output], source->branches[branch].trace,
                                   source->branches[branch].trace_length, output);
      }
    }
    for (size_t output = 0U; output < automaton->output_count; ++output) {
      sg_bitset_free(&groups[output]);
    }
    free(groups);
    free(used);
    if (status != SG_OK) {
      sg_trace_partition_free(destination);
      return status;
    }
  }
  return SG_OK;
}

static sg_status sg_trace_partition_apply_word(const sg_automaton *automaton,
                                               const sg_trace_partition *source,
                                               const sg_word *word,
                                               sg_trace_partition *destination) {
  sg_trace_partition current = {0};
  if (source->count != 1U || source->branches[0].trace_length != 0U) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  sg_status status = sg_trace_partition_init(&source->branches[0].support, &current);
  for (size_t index = 0U; status == SG_OK && index < word->length; ++index) {
    sg_trace_partition next = {0};
    status = sg_trace_partition_apply_action(automaton, &current, word->actions[index], &next);
    sg_trace_partition_free(&current);
    current = next;
  }
  if (status != SG_OK) {
    sg_trace_partition_free(&current);
    return status;
  }
  *destination = current;
  return SG_OK;
}

static void sg_trace_metrics(const sg_trace_partition *partition, size_t *best, size_t *worst,
                             size_t *total) {
  *best = SG_INDEX_NONE;
  *worst = 0U;
  *total = 0U;
  for (size_t index = 0U; index < partition->count; ++index) {
    const size_t count = sg_bitset_count(&partition->branches[index].support);
    if (count < *best) {
      *best = count;
    }
    if (count > *worst) {
      *worst = count;
    }
    *total += count;
  }
  if (partition->count == 0U) {
    *best = 0U;
  }
}

static sg_status sg_fill_plan_metrics(const sg_automaton *automaton, const sg_bitset *initial,
                                      sg_plan_result *result) {
  sg_trace_partition source = {0};
  sg_trace_partition final = {0};
  sg_status status = sg_trace_partition_init(initial, &source);
  if (status == SG_OK) {
    status = sg_trace_partition_apply_word(automaton, &source, &result->word, &final);
  }
  if (status == SG_OK) {
    size_t total = 0U;
    sg_trace_metrics(&final, &result->best_support_size, &result->worst_support_size, &total);
    result->branch_count = final.count;
    result->homing = result->worst_support_size <= 1U;
  }
  sg_trace_partition_free(&source);
  sg_trace_partition_free(&final);
  return status;
}

static sg_status sg_sync_finalize(const sg_automaton *automaton, const sg_bitset *initial,
                                  sg_plan_result *result) {
  sg_bitset final = {0};
  sg_status status = sg_apply_word_set(automaton, initial, &result->word, &final);
  if (status == SG_OK) {
    result->final_support_size = sg_bitset_count(&final);
    if (result->final_support_size != 1U) {
      status = SG_ERR_INVALID_MODEL;
    } else {
      size_t state = 0U;
      size_t count = 0U;
      sg_bitset_to_ids(&final, &state, &count);
      result->final_state = state;
      status = sg_fill_plan_metrics(automaton, initial, result);
    }
  }
  sg_bitset_free(&final);
  return status;
}

sg_status sg_plan_sync(const sg_automaton *automaton, const sg_pair_oracle *oracle,
                       const size_t *initial_states, size_t initial_count, size_t budget,
                       sg_plan_result *result) {
  if (automaton == NULL || oracle == NULL || oracle->automaton != automaton ||
      initial_states == NULL || initial_count == 0U || budget == 0U || result == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  const uint64_t start = sg_monotonic_time_us();
  sg_status status = sg_plan_result_init(automaton, result);
  if (status != SG_OK) {
    return status;
  }
  sg_bitset initial = {0};
  sg_bitset active = {0};
  status = sg_bitset_from_ids(automaton->state_count, initial_states, initial_count, &initial);
  if (status == SG_OK) {
    status = sg_bitset_copy(&initial, &active);
  }
  if (status != SG_OK) {
    sg_plan_result_free(result);
    sg_bitset_free(&initial);
    return status;
  }
  if (sg_bitset_count(&active) == 1U) {
    result->outcome = SG_OUTCOME_ALREADY_SATISFIED;
    status = sg_sync_finalize(automaton, &initial, result);
  }
  while (status == SG_OK && result->outcome != SG_OUTCOME_ALREADY_SATISFIED &&
         sg_bitset_count(&active) > 1U) {
    if (result->expansions >= budget) {
      result->outcome = SG_OUTCOME_RESOURCE_BOUND;
      break;
    }
    sg_word witness = {0};
    sg_bitset next = {0};
    status = sg_best_merge(automaton, oracle, &active, &witness, &next);
    if (status == SG_ERR_NOT_FOUND) {
      result->outcome = SG_OUTCOME_NO_PLAN;
      status = SG_OK;
      break;
    }
    if (status == SG_OK) {
      status = sg_word_extend(&result->word, &witness);
    }
    sg_word_free(&witness);
    if (status == SG_OK) {
      sg_bitset_free(&active);
      active = next;
      ++result->expansions;
    } else {
      sg_bitset_free(&next);
    }
  }
  if (status == SG_OK && result->outcome != SG_OUTCOME_ALREADY_SATISFIED &&
      sg_bitset_count(&active) == 1U) {
    result->outcome = SG_OUTCOME_PLAN;
    result->method = SG_METHOD_PAIR_MERGE;
    status = sg_sync_finalize(automaton, &initial, result);
  }
  result->planning_time_us = sg_elapsed_us(start);
  sg_bitset_free(&initial);
  sg_bitset_free(&active);
  if (status != SG_OK) {
    sg_plan_result_free(result);
  }
  return status;
}

static bool sg_resolution_candidate_better(size_t worst, size_t length, size_t branches,
                                           size_t pair, size_t best_worst, size_t best_length,
                                           size_t best_branches, size_t best_pair) {
  return worst < best_worst ||
         (worst == best_worst &&
          (length < best_length ||
           (length == best_length &&
            (branches > best_branches || (branches == best_branches && pair < best_pair)))));
}

static sg_status sg_best_resolution(const sg_automaton *automaton, const sg_pair_oracle *oracle,
                                    const sg_bitset *initial, size_t current_worst,
                                    sg_word *best_word, sg_trace_partition *best_partition) {
  sg_trace_partition source = {0};
  sg_status status = sg_trace_partition_init(initial, &source);
  if (status != SG_OK) {
    return status;
  }
  size_t best_worst = SG_INDEX_NONE;
  size_t best_length = SG_INDEX_NONE;
  size_t best_branches = 0U;
  size_t best_pair = SG_INDEX_NONE;
  for (size_t first = 0U; first < automaton->state_count; ++first) {
    if (!sg_bitset_has(initial, first)) {
      continue;
    }
    for (size_t second = first + 1U; second < automaton->state_count; ++second) {
      if (!sg_bitset_has(initial, second)) {
        continue;
      }
      sg_word candidate_word = {0};
      if (sg_pair_oracle_resolution_word(oracle, first, second, &candidate_word) != SG_OK) {
        continue;
      }
      sg_trace_partition candidate_partition = {0};
      status =
          sg_trace_partition_apply_word(automaton, &source, &candidate_word, &candidate_partition);
      if (status != SG_OK) {
        sg_word_free(&candidate_word);
        break;
      }
      size_t best = 0U;
      size_t worst = 0U;
      size_t total = 0U;
      sg_trace_metrics(&candidate_partition, &best, &worst, &total);
      const size_t pair = sg_pair_index(automaton->state_count, first, second);
      if (sg_resolution_candidate_better(worst, candidate_word.length, candidate_partition.count,
                                         pair, best_worst, best_length, best_branches, best_pair)) {
        sg_word_free(best_word);
        sg_trace_partition_free(best_partition);
        *best_word = candidate_word;
        *best_partition = candidate_partition;
        best_worst = worst;
        best_length = candidate_word.length;
        best_branches = candidate_partition.count;
        best_pair = pair;
      } else {
        sg_word_free(&candidate_word);
        sg_trace_partition_free(&candidate_partition);
      }
    }
    if (status != SG_OK) {
      break;
    }
  }
  sg_trace_partition_free(&source);
  if (status != SG_OK) {
    sg_word_free(best_word);
    sg_trace_partition_free(best_partition);
    return status;
  }
  return best_pair != SG_INDEX_NONE && best_worst < current_worst ? SG_OK : SG_ERR_NOT_FOUND;
}

static void sg_support_partition_free(sg_support_partition *partition) {
  if (partition == NULL) {
    return;
  }
  for (size_t index = 0U; index < partition->count; ++index) {
    sg_bitset_free(&partition->supports[index]);
  }
  free(partition->supports);
  partition->supports = NULL;
  partition->count = 0U;
  partition->capacity = 0U;
}

static sg_status sg_support_partition_add(sg_support_partition *partition, sg_bitset *support) {
  for (size_t index = 0U; index < partition->count; ++index) {
    if (sg_bitset_equal(&partition->supports[index], support)) {
      sg_bitset_free(support);
      return SG_OK;
    }
  }
  if (partition->count == partition->capacity) {
    const size_t capacity = partition->capacity == 0U ? 8U : partition->capacity * 2U;
    size_t bytes = 0U;
    if (capacity < partition->capacity ||
        !sg_size_multiply(capacity, sizeof(*partition->supports), &bytes)) {
      return SG_ERR_ALLOC;
    }
    sg_bitset *supports = realloc(partition->supports, bytes);
    if (supports == NULL) {
      return SG_ERR_ALLOC;
    }
    partition->supports = supports;
    partition->capacity = capacity;
  }
  size_t position = partition->count;
  while (position > 0U && sg_bitset_compare(support, &partition->supports[position - 1U]) < 0) {
    partition->supports[position] = partition->supports[position - 1U];
    --position;
  }
  partition->supports[position] = *support;
  support->words = NULL;
  ++partition->count;
  return SG_OK;
}

static sg_status sg_support_partition_init(const sg_bitset *initial,
                                           sg_support_partition *partition) {
  sg_bitset copy = {0};
  sg_status status = sg_bitset_copy(initial, &copy);
  if (status == SG_OK) {
    status = sg_support_partition_add(partition, &copy);
  }
  sg_bitset_free(&copy);
  return status;
}

static size_t sg_support_partition_worst(const sg_support_partition *partition) {
  size_t worst = 0U;
  for (size_t index = 0U; index < partition->count; ++index) {
    const size_t count = sg_bitset_count(&partition->supports[index]);
    if (count > worst) {
      worst = count;
    }
  }
  return worst;
}

static bool sg_support_partition_equal(const sg_support_partition *first,
                                       const sg_support_partition *second) {
  if (first->count != second->count) {
    return false;
  }
  for (size_t index = 0U; index < first->count; ++index) {
    if (!sg_bitset_equal(&first->supports[index], &second->supports[index])) {
      return false;
    }
  }
  return true;
}

static sg_status sg_support_partition_apply_action(const sg_automaton *automaton,
                                                   const sg_support_partition *source,
                                                   size_t action,
                                                   sg_support_partition *destination) {
  for (size_t branch = 0U; branch < source->count; ++branch) {
    sg_bitset *groups = calloc(automaton->output_count, sizeof(*groups));
    bool *used = calloc(automaton->output_count, sizeof(*used));
    if (groups == NULL || used == NULL) {
      free(groups);
      free(used);
      sg_support_partition_free(destination);
      return SG_ERR_ALLOC;
    }
    sg_status status = SG_OK;
    for (size_t state = 0U; state < automaton->state_count; ++state) {
      if (!sg_bitset_has(&source->supports[branch], state)) {
        continue;
      }
      const size_t output = sg_automaton_observation(automaton, state, action);
      if (!used[output]) {
        status = sg_bitset_init(&groups[output], automaton->state_count);
        if (status != SG_OK) {
          break;
        }
        used[output] = true;
      }
      if (groups[output].words == NULL) {
        status = SG_ERR_INVALID_MODEL;
        break;
      }
      sg_bitset_add(&groups[output], sg_automaton_transition(automaton, state, action));
    }
    for (size_t output = 0U; status == SG_OK && output < automaton->output_count; ++output) {
      if (used[output]) {
        status = sg_support_partition_add(destination, &groups[output]);
      }
    }
    for (size_t output = 0U; output < automaton->output_count; ++output) {
      sg_bitset_free(&groups[output]);
    }
    free(groups);
    free(used);
    if (status != SG_OK) {
      sg_support_partition_free(destination);
      return status;
    }
  }
  return SG_OK;
}

static void sg_search_nodes_free(sg_search_nodes *search) {
  if (search == NULL) {
    return;
  }
  for (size_t index = 0U; index < search->count; ++index) {
    sg_support_partition_free(&search->nodes[index].partition);
  }
  free(search->nodes);
  search->nodes = NULL;
  search->count = 0U;
  search->capacity = 0U;
}

static sg_status sg_search_nodes_add(sg_search_nodes *search, sg_support_partition *partition,
                                     size_t parent, size_t action, size_t *index) {
  if (search->count == search->capacity) {
    const size_t capacity = search->capacity == 0U ? 16U : search->capacity * 2U;
    size_t bytes = 0U;
    if (capacity < search->capacity ||
        !sg_size_multiply(capacity, sizeof(*search->nodes), &bytes)) {
      return SG_ERR_ALLOC;
    }
    sg_search_node *nodes = realloc(search->nodes, bytes);
    if (nodes == NULL) {
      return SG_ERR_ALLOC;
    }
    search->nodes = nodes;
    search->capacity = capacity;
  }
  *index = search->count;
  search->nodes[search->count] = (sg_search_node){
      .partition = *partition,
      .parent = parent,
      .action = action,
  };
  partition->supports = NULL;
  partition->count = 0U;
  partition->capacity = 0U;
  ++search->count;
  return SG_OK;
}

static size_t sg_search_find(const sg_search_nodes *search, const sg_support_partition *partition) {
  for (size_t index = 0U; index < search->count; ++index) {
    if (sg_support_partition_equal(&search->nodes[index].partition, partition)) {
      return index;
    }
  }
  return SG_INDEX_NONE;
}

static sg_status sg_search_reconstruct(const sg_search_nodes *search, size_t goal, sg_word *word) {
  sg_status status = sg_word_init(word);
  size_t cursor = goal;
  while (status == SG_OK && search->nodes[cursor].parent != SG_INDEX_NONE) {
    status = sg_word_append(word, search->nodes[cursor].action);
    cursor = search->nodes[cursor].parent;
  }
  for (size_t first = 0U, second = word->length; first < second && second != 0U; ++first) {
    --second;
    const size_t temporary = word->actions[first];
    word->actions[first] = word->actions[second];
    word->actions[second] = temporary;
  }
  if (status != SG_OK) {
    sg_word_free(word);
  }
  return status;
}

static sg_status sg_partition_bfs(const sg_automaton *automaton, const sg_bitset *initial,
                                  size_t bound, size_t budget, size_t *expansions, sg_word *word,
                                  sg_plan_outcome *outcome) {
  sg_search_nodes search = {0};
  sg_support_partition root = {0};
  sg_status status = sg_support_partition_init(initial, &root);
  size_t root_index = 0U;
  if (status == SG_OK) {
    status = sg_search_nodes_add(&search, &root, SG_INDEX_NONE, SG_INDEX_NONE, &root_index);
  }
  size_t head = 0U;
  size_t goal = SG_INDEX_NONE;
  while (status == SG_OK && head < search.count && goal == SG_INDEX_NONE) {
    if (*expansions >= budget) {
      *outcome = SG_OUTCOME_RESOURCE_BOUND;
      break;
    }
    const size_t parent = head;
    ++head;
    ++*expansions;
    for (size_t action = 0U; action < automaton->action_count; ++action) {
      sg_support_partition next = {0};
      status = sg_support_partition_apply_action(automaton, &search.nodes[parent].partition, action,
                                                 &next);
      if (status != SG_OK) {
        break;
      }
      if (sg_search_find(&search, &next) != SG_INDEX_NONE) {
        sg_support_partition_free(&next);
        continue;
      }
      size_t next_index = 0U;
      status = sg_search_nodes_add(&search, &next, parent, action, &next_index);
      if (status != SG_OK) {
        sg_support_partition_free(&next);
        break;
      }
      if (sg_support_partition_worst(&search.nodes[next_index].partition) <= bound) {
        goal = next_index;
        break;
      }
    }
  }
  if (status == SG_OK && goal != SG_INDEX_NONE) {
    status = sg_search_reconstruct(&search, goal, word);
    *outcome = SG_OUTCOME_PLAN;
  } else if (status == SG_OK && *outcome != SG_OUTCOME_RESOURCE_BOUND) {
    *outcome = SG_OUTCOME_NO_PLAN;
  }
  sg_support_partition_free(&root);
  sg_search_nodes_free(&search);
  return status;
}

static sg_status sg_disambiguation_finalize(const sg_automaton *automaton, const sg_bitset *initial,
                                            size_t bound, sg_plan_result *result) {
  sg_status status = sg_fill_plan_metrics(automaton, initial, result);
  if (status == SG_OK && result->worst_support_size > bound) {
    return SG_ERR_INVALID_MODEL;
  }
  result->final_support_size = result->worst_support_size;
  return status;
}

sg_status sg_plan_disambiguate(const sg_automaton *automaton, const sg_pair_oracle *oracle,
                               const size_t *initial_states, size_t initial_count, size_t bound,
                               size_t budget, sg_plan_result *result) {
  if (automaton == NULL || oracle == NULL || oracle->automaton != automaton ||
      initial_states == NULL || initial_count == 0U || bound == 0U || budget == 0U ||
      result == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  const uint64_t start = sg_monotonic_time_us();
  sg_status status = sg_plan_result_init(automaton, result);
  if (status != SG_OK) {
    return status;
  }
  sg_bitset initial = {0};
  status = sg_bitset_from_ids(automaton->state_count, initial_states, initial_count, &initial);
  const size_t unique_count = status == SG_OK ? sg_bitset_count(&initial) : 0U;
  if (status != SG_OK || bound > unique_count) {
    sg_plan_result_free(result);
    sg_bitset_free(&initial);
    return status == SG_OK ? SG_ERR_INVALID_ARGUMENT : status;
  }
  if (unique_count <= bound) {
    result->outcome = SG_OUTCOME_ALREADY_SATISFIED;
    status = sg_disambiguation_finalize(automaton, &initial, bound, result);
  } else {
    sg_word heuristic = {0};
    sg_trace_partition heuristic_partition = {0};
    status = sg_best_resolution(automaton, oracle, &initial, unique_count, &heuristic,
                                &heuristic_partition);
    if (status == SG_OK) {
      size_t best = 0U;
      size_t worst = 0U;
      size_t total = 0U;
      sg_trace_metrics(&heuristic_partition, &best, &worst, &total);
      ++result->expansions;
      if (worst <= bound) {
        result->word = heuristic;
        result->outcome = SG_OUTCOME_PLAN;
        result->method = SG_METHOD_PAIR_RESOLUTION;
        status = sg_disambiguation_finalize(automaton, &initial, bound, result);
      } else {
        sg_word_free(&heuristic);
        status = SG_ERR_NOT_FOUND;
      }
    }
    sg_trace_partition_free(&heuristic_partition);
    if (status == SG_ERR_NOT_FOUND) {
      status = sg_partition_bfs(automaton, &initial, bound, budget, &result->expansions,
                                &result->word, &result->outcome);
      if (status == SG_OK && result->outcome == SG_OUTCOME_PLAN) {
        result->method = SG_METHOD_PARTITION_BFS;
        status = sg_disambiguation_finalize(automaton, &initial, bound, result);
      }
    }
  }
  result->planning_time_us = sg_elapsed_us(start);
  sg_bitset_free(&initial);
  if (status != SG_OK) {
    sg_plan_result_free(result);
  }
  return status;
}

static sg_status sg_explain_partition(const sg_trace_partition *partition,
                                      const sg_bitset *predicted, size_t step, size_t action,
                                      sg_explain_visitor visitor, void *context) {
  const size_t predicted_count = sg_bitset_count(predicted);
  size_t *predicted_states = malloc(predicted_count * sizeof(*predicted_states));
  if (predicted_states == NULL) {
    return SG_ERR_ALLOC;
  }
  size_t converted = 0U;
  sg_bitset_to_ids(predicted, predicted_states, &converted);
  sg_status status = SG_OK;
  for (size_t branch = 0U; status == SG_OK && branch < partition->count; ++branch) {
    const size_t branch_count = sg_bitset_count(&partition->branches[branch].support);
    size_t *branch_states = malloc(branch_count * sizeof(*branch_states));
    if (branch_states == NULL) {
      status = SG_ERR_ALLOC;
      break;
    }
    sg_bitset_to_ids(&partition->branches[branch].support, branch_states, &converted);
    status = visitor(context, step, action, predicted_states, predicted_count,
                     partition->branches[branch].trace, partition->branches[branch].trace_length,
                     branch_states, branch_count);
    free(branch_states);
  }
  free(predicted_states);
  return status;
}

sg_status sg_explain_plan(const sg_automaton *automaton, uint64_t plan_generation,
                          const size_t *initial_states, size_t initial_count, const sg_word *word,
                          sg_explain_visitor visitor, void *context) {
  if (automaton == NULL || initial_states == NULL || initial_count == 0U || word == NULL ||
      visitor == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  if (plan_generation != automaton->generation) {
    return SG_ERR_STALE_GENERATION;
  }
  sg_bitset predicted = {0};
  sg_bitset next_predicted = {0};
  sg_trace_partition partition = {0};
  sg_status status =
      sg_bitset_from_ids(automaton->state_count, initial_states, initial_count, &predicted);
  if (status == SG_OK) {
    status = sg_bitset_init(&next_predicted, automaton->state_count);
  }
  if (status == SG_OK) {
    status = sg_trace_partition_init(&predicted, &partition);
  }
  if (status == SG_OK) {
    status = sg_explain_partition(&partition, &predicted, 0U, SG_INDEX_NONE, visitor, context);
  }
  for (size_t index = 0U; status == SG_OK && index < word->length; ++index) {
    if (word->actions[index] >= automaton->action_count) {
      status = SG_ERR_INVALID_ARGUMENT;
      break;
    }
    sg_apply_action_set(automaton, &predicted, word->actions[index], &next_predicted);
    sg_bitset temporary = predicted;
    predicted = next_predicted;
    next_predicted = temporary;
    sg_trace_partition next_partition = {0};
    status = sg_trace_partition_apply_action(automaton, &partition, word->actions[index],
                                             &next_partition);
    sg_trace_partition_free(&partition);
    partition = next_partition;
    if (status == SG_OK) {
      status = sg_explain_partition(&partition, &predicted, index + 1U, word->actions[index],
                                    visitor, context);
    }
  }
  sg_bitset_free(&predicted);
  sg_bitset_free(&next_predicted);
  sg_trace_partition_free(&partition);
  return status;
}

void sg_monitor_result_free(sg_monitor_result *result) {
  if (result == NULL) {
    return;
  }
  free(result->expected_states);
  free(result->unexpected_states);
  result->expected_states = NULL;
  result->unexpected_states = NULL;
  result->expected_count = 0U;
  result->unexpected_count = 0U;
  result->generation = 0U;
  result->decision = SG_MONITOR_WAIT;
}

static sg_status sg_monitor_fill_arrays(const sg_bitset *expected, const sg_bitset *reported,
                                        sg_monitor_result *result) {
  result->expected_count = sg_bitset_count(expected);
  result->expected_states = malloc(result->expected_count * sizeof(*result->expected_states));
  if (result->expected_states == NULL) {
    return SG_ERR_ALLOC;
  }
  size_t converted = 0U;
  sg_bitset_to_ids(expected, result->expected_states, &converted);
  sg_bitset unexpected = {0};
  sg_status status = sg_bitset_init(&unexpected, expected->state_count);
  if (status != SG_OK) {
    return status;
  }
  for (size_t state = 0U; state < expected->state_count; ++state) {
    if (sg_bitset_has(reported, state) && !sg_bitset_has(expected, state)) {
      sg_bitset_add(&unexpected, state);
    }
  }
  result->unexpected_count = sg_bitset_count(&unexpected);
  if (result->unexpected_count != 0U) {
    result->unexpected_states =
        malloc(result->unexpected_count * sizeof(*result->unexpected_states));
    if (result->unexpected_states == NULL) {
      sg_bitset_free(&unexpected);
      return SG_ERR_ALLOC;
    }
    sg_bitset_to_ids(&unexpected, result->unexpected_states, &converted);
  }
  sg_bitset_free(&unexpected);
  return SG_OK;
}

sg_status sg_validate_update(const sg_automaton *automaton, uint64_t plan_generation,
                             const size_t *initial_states, size_t initial_count,
                             const sg_word *word, size_t completed_steps,
                             const size_t *reported_states, size_t reported_count,
                             bool localizer_available, sg_monitor_result *result) {
  if (automaton == NULL || initial_states == NULL || initial_count == 0U || word == NULL ||
      completed_steps > word->length || result == NULL ||
      (localizer_available && (reported_states == NULL || reported_count == 0U))) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *result = (sg_monitor_result){.decision = SG_MONITOR_WAIT, .generation = automaton->generation};
  if (plan_generation != automaton->generation) {
    result->decision = SG_MONITOR_STALE_GENERATION;
    return SG_OK;
  }
  sg_bitset expected = {0};
  sg_bitset next = {0};
  sg_status status =
      sg_bitset_from_ids(automaton->state_count, initial_states, initial_count, &expected);
  if (status == SG_OK) {
    status = sg_bitset_init(&next, automaton->state_count);
  }
  for (size_t index = 0U; status == SG_OK && index < completed_steps; ++index) {
    if (word->actions[index] >= automaton->action_count) {
      status = SG_ERR_INVALID_ARGUMENT;
      break;
    }
    sg_apply_action_set(automaton, &expected, word->actions[index], &next);
    sg_bitset temporary = expected;
    expected = next;
    next = temporary;
  }
  if (status != SG_OK) {
    sg_bitset_free(&expected);
    sg_bitset_free(&next);
    return status;
  }
  if (!localizer_available) {
    result->expected_count = sg_bitset_count(&expected);
    if (result->expected_count == 0U) {
      status = SG_ERR_INVALID_MODEL;
    } else {
      result->expected_states = calloc(result->expected_count, sizeof(*result->expected_states));
      if (result->expected_states == NULL) {
        status = SG_ERR_ALLOC;
      } else {
        size_t converted = 0U;
        sg_bitset_to_ids(&expected, result->expected_states, &converted);
      }
    }
  } else {
    sg_bitset reported = {0};
    status = sg_bitset_from_ids(automaton->state_count, reported_states, reported_count, &reported);
    if (status == SG_OK) {
      status = sg_monitor_fill_arrays(&expected, &reported, result);
    }
    if (status == SG_OK) {
      if (sg_bitset_equal(&reported, &expected)) {
        result->decision = SG_MONITOR_CONTINUE;
      } else if (sg_bitset_subset(&reported, &expected)) {
        result->decision = SG_MONITOR_REPLAN;
      } else {
        result->decision = SG_MONITOR_MODEL_VIOLATION;
      }
    }
    sg_bitset_free(&reported);
  }
  sg_bitset_free(&expected);
  sg_bitset_free(&next);
  if (status != SG_OK) {
    sg_monitor_result_free(result);
  }
  return status;
}
