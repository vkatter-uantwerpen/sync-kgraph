#include "sync_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  char *source;
  char *action;
  char *value;
} sg_builder_entry;

struct sg_automaton_builder {
  char **states;
  size_t state_count;
  size_t state_capacity;
  char **actions;
  size_t action_count;
  size_t action_capacity;
  char **outputs;
  size_t output_count;
  size_t output_capacity;
  sg_builder_entry *transitions;
  size_t transition_count;
  size_t transition_capacity;
  sg_builder_entry *observations;
  size_t observation_count;
  size_t observation_capacity;
};

bool sg_size_multiply(size_t first, size_t second, size_t *product) {
  if (product == NULL || (first != 0U && second > (SIZE_MAX / first))) {
    return false;
  }
  *product = first * second;
  return true;
}

char *sg_string_duplicate(const char *value) {
  if (value == NULL) {
    return NULL;
  }
  const size_t length = strlen(value);
  if (length == SIZE_MAX) {
    return NULL;
  }
  char *copy = malloc(length + 1U);
  if (copy != NULL) {
    memcpy(copy, value, length + 1U);
  }
  return copy;
}

uint64_t sg_monotonic_time_us(void) {
  struct timespec value = {0};
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
    return 0U;
  }
  const uint64_t seconds = (uint64_t)value.tv_sec;
  const uint64_t nanoseconds = (uint64_t)value.tv_nsec;
  if (seconds > (UINT64_MAX / UINT64_C(1000000))) {
    return UINT64_MAX;
  }
  return (seconds * UINT64_C(1000000)) + (nanoseconds / UINT64_C(1000));
}

const char *sg_status_name(sg_status status) {
  switch (status) {
  case SG_OK:
    return "OK";
  case SG_ERR_ALLOC:
    return "ALLOC";
  case SG_ERR_INVALID_ARGUMENT:
    return "INVALID_ARGUMENT";
  case SG_ERR_DUPLICATE:
    return "DUPLICATE";
  case SG_ERR_NOT_FOUND:
    return "NOT_FOUND";
  case SG_ERR_INCOMPLETE:
    return "INCOMPLETE";
  case SG_ERR_NONDETERMINISTIC:
    return "NONDETERMINISTIC";
  case SG_ERR_INVALID_MODEL:
    return "INVALID_MODEL";
  case SG_ERR_RESOURCE_BOUND:
    return "RESOURCE_BOUND";
  case SG_ERR_STALE_GENERATION:
    return "STALE_GENERATION";
  }
  return "UNKNOWN";
}

const char *sg_plan_outcome_name(sg_plan_outcome outcome) {
  switch (outcome) {
  case SG_OUTCOME_PLAN:
    return "PLAN";
  case SG_OUTCOME_ALREADY_SATISFIED:
    return "ALREADY_SATISFIED";
  case SG_OUTCOME_NO_PLAN:
    return "NO_PLAN";
  case SG_OUTCOME_RESOURCE_BOUND:
    return "RESOURCE_BOUND";
  }
  return "UNKNOWN";
}

const char *sg_plan_method_name(sg_plan_method method) {
  switch (method) {
  case SG_METHOD_NONE:
    return "NONE";
  case SG_METHOD_PAIR_MERGE:
    return "PAIR_MERGE";
  case SG_METHOD_PAIR_RESOLUTION:
    return "PAIR_RESOLUTION";
  case SG_METHOD_PARTITION_BFS:
    return "PARTITION_BFS";
  case SG_METHOD_BELIEF_BFS:
    return "BELIEF_BFS";
  }
  return "UNKNOWN";
}

const char *sg_monitor_decision_name(sg_monitor_decision decision) {
  switch (decision) {
  case SG_MONITOR_CONTINUE:
    return "CONTINUE";
  case SG_MONITOR_REPLAN:
    return "REPLAN";
  case SG_MONITOR_MODEL_VIOLATION:
    return "MODEL_VIOLATION";
  case SG_MONITOR_STALE_GENERATION:
    return "STALE_GENERATION";
  case SG_MONITOR_WAIT:
    return "WAIT";
  }
  return "UNKNOWN";
}

sg_status sg_word_init(sg_word *word) {
  if (word == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  word->actions = NULL;
  word->length = 0U;
  word->capacity = 0U;
  return SG_OK;
}

void sg_word_free(sg_word *word) {
  if (word == NULL) {
    return;
  }
  free(word->actions);
  word->actions = NULL;
  word->length = 0U;
  word->capacity = 0U;
}

static sg_status sg_word_reserve(sg_word *word, size_t capacity) {
  if (capacity <= word->capacity) {
    return SG_OK;
  }
  size_t bytes = 0U;
  if (!sg_size_multiply(capacity, sizeof(*word->actions), &bytes)) {
    return SG_ERR_ALLOC;
  }
  size_t *actions = realloc(word->actions, bytes);
  if (actions == NULL) {
    return SG_ERR_ALLOC;
  }
  word->actions = actions;
  word->capacity = capacity;
  return SG_OK;
}

sg_status sg_word_append(sg_word *word, size_t action) {
  if (word == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  if (word->length == word->capacity) {
    const size_t capacity = word->capacity == 0U ? 8U : word->capacity * 2U;
    if (capacity < word->capacity) {
      return SG_ERR_ALLOC;
    }
    const sg_status status = sg_word_reserve(word, capacity);
    if (status != SG_OK) {
      return status;
    }
  }
  word->actions[word->length] = action;
  ++word->length;
  return SG_OK;
}

sg_status sg_word_extend(sg_word *destination, const sg_word *source) {
  if (destination == NULL || source == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  if (source->length > (SIZE_MAX - destination->length)) {
    return SG_ERR_ALLOC;
  }
  const size_t required = destination->length + source->length;
  sg_status status = sg_word_reserve(destination, required);
  if (status != SG_OK) {
    return status;
  }
  if (source->length != 0U) {
    memcpy(&destination->actions[destination->length], source->actions,
           source->length * sizeof(*source->actions));
  }
  destination->length = required;
  return SG_OK;
}

static void sg_string_array_free(char **values, size_t count) {
  if (values == NULL) {
    return;
  }
  for (size_t index = 0U; index < count; ++index) {
    free(values[index]);
  }
  free(values);
}

static void sg_builder_entries_free(sg_builder_entry *entries, size_t count) {
  if (entries == NULL) {
    return;
  }
  for (size_t index = 0U; index < count; ++index) {
    free(entries[index].source);
    free(entries[index].action);
    free(entries[index].value);
  }
  free(entries);
}

sg_status sg_automaton_builder_init(sg_automaton_builder **builder) {
  if (builder == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *builder = calloc(1U, sizeof(**builder));
  return *builder == NULL ? SG_ERR_ALLOC : SG_OK;
}

void sg_automaton_builder_free(sg_automaton_builder *builder) {
  if (builder == NULL) {
    return;
  }
  sg_string_array_free(builder->states, builder->state_count);
  sg_string_array_free(builder->actions, builder->action_count);
  sg_string_array_free(builder->outputs, builder->output_count);
  sg_builder_entries_free(builder->transitions, builder->transition_count);
  sg_builder_entries_free(builder->observations, builder->observation_count);
  free(builder);
}

static size_t sg_string_array_find(char *const *values, size_t count, const char *value) {
  for (size_t index = 0U; index < count; ++index) {
    if (strcmp(values[index], value) == 0) {
      return index;
    }
  }
  return SG_INDEX_NONE;
}

static sg_status sg_string_array_add(char ***values, size_t *count, size_t *capacity,
                                     const char *value) {
  if (values == NULL || count == NULL || capacity == NULL || value == NULL || value[0] == '\0') {
    return SG_ERR_INVALID_ARGUMENT;
  }
  if (sg_string_array_find(*values, *count, value) != SG_INDEX_NONE) {
    return SG_ERR_DUPLICATE;
  }
  if (*count == *capacity) {
    const size_t next_capacity = *capacity == 0U ? 8U : *capacity * 2U;
    if (next_capacity < *capacity) {
      return SG_ERR_ALLOC;
    }
    size_t bytes = 0U;
    if (!sg_size_multiply(next_capacity, sizeof(**values), &bytes)) {
      return SG_ERR_ALLOC;
    }
    char **next = realloc(*values, bytes);
    if (next == NULL) {
      return SG_ERR_ALLOC;
    }
    *values = next;
    *capacity = next_capacity;
  }
  char *copy = sg_string_duplicate(value);
  if (copy == NULL) {
    return SG_ERR_ALLOC;
  }
  (*values)[*count] = copy;
  ++*count;
  return SG_OK;
}

sg_status sg_automaton_builder_add_state(sg_automaton_builder *builder, const char *state_key) {
  if (builder == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  return sg_string_array_add(&builder->states, &builder->state_count, &builder->state_capacity,
                             state_key);
}

sg_status sg_automaton_builder_add_action(sg_automaton_builder *builder, const char *action_key) {
  if (builder == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  return sg_string_array_add(&builder->actions, &builder->action_count, &builder->action_capacity,
                             action_key);
}

sg_status sg_automaton_builder_add_output(sg_automaton_builder *builder, const char *output_key) {
  if (builder == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  return sg_string_array_add(&builder->outputs, &builder->output_count, &builder->output_capacity,
                             output_key);
}

static sg_status sg_builder_entry_add(sg_builder_entry **entries, size_t *count, size_t *capacity,
                                      const char *source, const char *action, const char *value) {
  if (entries == NULL || count == NULL || capacity == NULL || source == NULL || action == NULL ||
      value == NULL || source[0] == '\0' || action[0] == '\0' || value[0] == '\0') {
    return SG_ERR_INVALID_ARGUMENT;
  }
  if (*count == *capacity) {
    const size_t next_capacity = *capacity == 0U ? 16U : *capacity * 2U;
    if (next_capacity < *capacity) {
      return SG_ERR_ALLOC;
    }
    size_t bytes = 0U;
    if (!sg_size_multiply(next_capacity, sizeof(**entries), &bytes)) {
      return SG_ERR_ALLOC;
    }
    sg_builder_entry *next = realloc(*entries, bytes);
    if (next == NULL) {
      return SG_ERR_ALLOC;
    }
    *entries = next;
    *capacity = next_capacity;
  }
  sg_builder_entry entry = {
      .source = sg_string_duplicate(source),
      .action = sg_string_duplicate(action),
      .value = sg_string_duplicate(value),
  };
  if (entry.source == NULL || entry.action == NULL || entry.value == NULL) {
    free(entry.source);
    free(entry.action);
    free(entry.value);
    return SG_ERR_ALLOC;
  }
  (*entries)[*count] = entry;
  ++*count;
  return SG_OK;
}

sg_status sg_automaton_builder_add_transition(sg_automaton_builder *builder, const char *source_key,
                                              const char *action_key, const char *target_key) {
  if (builder == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  return sg_builder_entry_add(&builder->transitions, &builder->transition_count,
                              &builder->transition_capacity, source_key, action_key, target_key);
}

sg_status sg_automaton_builder_add_observation(sg_automaton_builder *builder,
                                               const char *source_key, const char *action_key,
                                               const char *output_key) {
  if (builder == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  return sg_builder_entry_add(&builder->observations, &builder->observation_count,
                              &builder->observation_capacity, source_key, action_key, output_key);
}

static sg_status sg_copy_keys(char *const *source, size_t count, char ***destination) {
  char **keys = calloc(count, sizeof(*keys));
  if (keys == NULL) {
    return SG_ERR_ALLOC;
  }
  for (size_t index = 0U; index < count; ++index) {
    keys[index] = sg_string_duplicate(source[index]);
    if (keys[index] == NULL) {
      sg_string_array_free(keys, count);
      return SG_ERR_ALLOC;
    }
  }
  *destination = keys;
  return SG_OK;
}

static sg_status sg_resolve_entries(const sg_automaton_builder *builder,
                                    const sg_builder_entry *entries, size_t entry_count,
                                    char *const *value_keys, size_t value_count, size_t *table) {
  for (size_t index = 0U; index < entry_count; ++index) {
    const size_t source =
        sg_string_array_find(builder->states, builder->state_count, entries[index].source);
    const size_t action =
        sg_string_array_find(builder->actions, builder->action_count, entries[index].action);
    const size_t value = sg_string_array_find(value_keys, value_count, entries[index].value);
    if (source == SG_INDEX_NONE || action == SG_INDEX_NONE || value == SG_INDEX_NONE) {
      return SG_ERR_NOT_FOUND;
    }
    const size_t cell = (source * builder->action_count) + action;
    if (table[cell] != SG_INDEX_NONE) {
      return table[cell] == value ? SG_ERR_DUPLICATE : SG_ERR_NONDETERMINISTIC;
    }
    table[cell] = value;
  }
  return SG_OK;
}

static sg_status sg_automaton_allocate(const sg_automaton_builder *builder, uint64_t generation,
                                       sg_automaton **automaton) {
  sg_automaton *created = calloc(1U, sizeof(*created));
  if (created == NULL) {
    return SG_ERR_ALLOC;
  }
  created->generation = generation;
  created->state_count = builder->state_count;
  created->action_count = builder->action_count;
  created->output_count = builder->output_count;
  size_t cells = 0U;
  if (!sg_size_multiply(created->state_count, created->action_count, &cells)) {
    sg_automaton_free(created);
    return SG_ERR_ALLOC;
  }
  created->transitions = malloc(cells * sizeof(*created->transitions));
  created->observations = malloc(cells * sizeof(*created->observations));
  if (created->transitions == NULL || created->observations == NULL) {
    sg_automaton_free(created);
    return SG_ERR_ALLOC;
  }
  for (size_t cell = 0U; cell < cells; ++cell) {
    created->transitions[cell] = SG_INDEX_NONE;
    created->observations[cell] = SG_INDEX_NONE;
  }
  *automaton = created;
  return SG_OK;
}

sg_status sg_automaton_builder_build(sg_automaton_builder *builder, uint64_t generation,
                                     sg_automaton **automaton) {
  if (builder == NULL || automaton == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *automaton = NULL;
  if (builder->state_count == 0U || builder->action_count == 0U || builder->output_count == 0U) {
    return SG_ERR_INCOMPLETE;
  }
  sg_automaton *created = NULL;
  sg_status status = sg_automaton_allocate(builder, generation, &created);
  if (status != SG_OK) {
    return status;
  }
  status = sg_copy_keys(builder->states, builder->state_count, &created->state_keys);
  if (status == SG_OK) {
    status = sg_copy_keys(builder->actions, builder->action_count, &created->action_keys);
  }
  if (status == SG_OK) {
    status = sg_copy_keys(builder->outputs, builder->output_count, &created->output_keys);
  }
  if (status == SG_OK) {
    status = sg_resolve_entries(builder, builder->transitions, builder->transition_count,
                                builder->states, builder->state_count, created->transitions);
  }
  if (status == SG_OK) {
    status = sg_resolve_entries(builder, builder->observations, builder->observation_count,
                                builder->outputs, builder->output_count, created->observations);
  }
  size_t cells = 0U;
  (void)sg_size_multiply(created->state_count, created->action_count, &cells);
  for (size_t cell = 0U; status == SG_OK && cell < cells; ++cell) {
    if (created->transitions[cell] == SG_INDEX_NONE ||
        created->observations[cell] == SG_INDEX_NONE) {
      status = SG_ERR_INCOMPLETE;
    }
  }
  if (status != SG_OK) {
    sg_automaton_free(created);
    return status;
  }
  *automaton = created;
  return SG_OK;
}

void sg_automaton_free(sg_automaton *automaton) {
  if (automaton == NULL) {
    return;
  }
  sg_string_array_free(automaton->state_keys, automaton->state_count);
  sg_string_array_free(automaton->action_keys, automaton->action_count);
  sg_string_array_free(automaton->output_keys, automaton->output_count);
  free(automaton->transitions);
  free(automaton->observations);
  free(automaton);
}

uint64_t sg_automaton_generation(const sg_automaton *automaton) {
  return automaton == NULL ? 0U : automaton->generation;
}

size_t sg_automaton_state_count(const sg_automaton *automaton) {
  return automaton == NULL ? 0U : automaton->state_count;
}

size_t sg_automaton_action_count(const sg_automaton *automaton) {
  return automaton == NULL ? 0U : automaton->action_count;
}

size_t sg_automaton_output_count(const sg_automaton *automaton) {
  return automaton == NULL ? 0U : automaton->output_count;
}

size_t sg_automaton_transition_count(const sg_automaton *automaton) {
  if (automaton == NULL) {
    return 0U;
  }
  return automaton->state_count * automaton->action_count;
}

const char *sg_automaton_state_key(const sg_automaton *automaton, size_t state) {
  return automaton == NULL || state >= automaton->state_count ? NULL : automaton->state_keys[state];
}

const char *sg_automaton_action_key(const sg_automaton *automaton, size_t action) {
  return automaton == NULL || action >= automaton->action_count ? NULL
                                                                : automaton->action_keys[action];
}

const char *sg_automaton_output_key(const sg_automaton *automaton, size_t output) {
  return automaton == NULL || output >= automaton->output_count ? NULL
                                                                : automaton->output_keys[output];
}

size_t sg_automaton_transition(const sg_automaton *automaton, size_t state, size_t action) {
  if (automaton == NULL || state >= automaton->state_count || action >= automaton->action_count) {
    return SG_INDEX_NONE;
  }
  return automaton->transitions[(state * automaton->action_count) + action];
}

size_t sg_automaton_observation(const sg_automaton *automaton, size_t state, size_t action) {
  if (automaton == NULL || state >= automaton->state_count || action >= automaton->action_count) {
    return SG_INDEX_NONE;
  }
  return automaton->observations[(state * automaton->action_count) + action];
}

static sg_status sg_find_key(char *const *keys, size_t count, const char *key, size_t *identifier) {
  if (keys == NULL || key == NULL || identifier == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  const size_t found = sg_string_array_find(keys, count, key);
  if (found == SG_INDEX_NONE) {
    return SG_ERR_NOT_FOUND;
  }
  *identifier = found;
  return SG_OK;
}

sg_status sg_automaton_find_state(const sg_automaton *automaton, const char *state_key,
                                  size_t *state) {
  return automaton == NULL
             ? SG_ERR_INVALID_ARGUMENT
             : sg_find_key(automaton->state_keys, automaton->state_count, state_key, state);
}

sg_status sg_automaton_find_action(const sg_automaton *automaton, const char *action_key,
                                   size_t *action) {
  return automaton == NULL
             ? SG_ERR_INVALID_ARGUMENT
             : sg_find_key(automaton->action_keys, automaton->action_count, action_key, action);
}

sg_status sg_automaton_find_output(const sg_automaton *automaton, const char *output_key,
                                   size_t *output) {
  return automaton == NULL
             ? SG_ERR_INVALID_ARGUMENT
             : sg_find_key(automaton->output_keys, automaton->output_count, output_key, output);
}
