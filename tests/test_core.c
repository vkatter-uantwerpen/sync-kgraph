#include "sync_kgraph/sync.h"

#include "dynamic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);              \
      exit(EXIT_FAILURE);                                                                          \
    }                                                                                              \
  } while (0)

enum {
  NUMERIC_STATE_COUNT = 6,
  NUMERIC_ACTION_COUNT = 3,
  NUMERIC_OUTPUT_COUNT = 2,
  NUMERIC_CELL_COUNT = NUMERIC_STATE_COUNT * NUMERIC_ACTION_COUNT,
  NUMERIC_MUTATION_COUNT = 48,
  NUMERIC_MUTATION_STRIDE = 5,
  NUMERIC_ACTION_STRIDE = 7,
};

typedef struct {
  const char *source;
  const char *action;
  const char *target;
  const char *output;
} machine_cell;

typedef struct {
  size_t calls;
  size_t initial_rows;
  size_t action_rows;
  size_t singleton_rows;
} explain_counts;

typedef struct {
  size_t state_count;
  size_t action_count;
  size_t pair_count;
  size_t *first_states;
  size_t *second_states;
  sg_pair_record *records;
  sg_pair_arc *arcs;
  size_t record_reads;
} memory_pair_store;

static sg_status memory_read_records(void *context, const size_t *pair_ids, size_t pair_count,
                                     sg_pair_record *records) {
  memory_pair_store *store = context;
  ++store->record_reads;
  for (size_t index = 0U; index < pair_count; ++index) {
    if (pair_ids[index] >= store->pair_count) {
      return SG_ERR_INVALID_ARGUMENT;
    }
    records[index] = store->records[pair_ids[index]];
  }
  return SG_OK;
}

static sg_status memory_read_outgoing(void *context, const size_t *source_pairs,
                                      size_t source_count, sg_pair_arc_batch *batch) {
  memory_pair_store *store = context;
  batch->count = source_count * store->action_count;
  batch->items = calloc(batch->count == 0U ? 1U : batch->count, sizeof(*batch->items));
  if (batch->items == NULL) {
    return SG_ERR_ALLOC;
  }
  size_t position = 0U;
  for (size_t source = 0U; source < source_count; ++source) {
    if (source_pairs[source] >= store->pair_count) {
      free(batch->items);
      *batch = (sg_pair_arc_batch){0};
      return SG_ERR_INVALID_ARGUMENT;
    }
    for (size_t action = 0U; action < store->action_count; ++action) {
      batch->items[position] = store->arcs[(source_pairs[source] * store->action_count) + action];
      ++position;
    }
  }
  return SG_OK;
}

static sg_status memory_read_incoming(void *context, const size_t *target_pairs,
                                      size_t target_count, sg_pair_arc_batch *batch) {
  memory_pair_store *store = context;
  bool *targets = calloc(store->pair_count, sizeof(*targets));
  if (targets == NULL) {
    return SG_ERR_ALLOC;
  }
  for (size_t index = 0U; index < target_count; ++index) {
    if (target_pairs[index] >= store->pair_count) {
      free(targets);
      return SG_ERR_INVALID_ARGUMENT;
    }
    targets[target_pairs[index]] = true;
  }
  size_t count = 0U;
  for (size_t edge = 0U; edge < store->pair_count * store->action_count; ++edge) {
    if (targets[store->arcs[edge].target_pair]) {
      ++count;
    }
  }
  batch->items = calloc(count == 0U ? 1U : count, sizeof(*batch->items));
  if (batch->items == NULL) {
    free(targets);
    return SG_ERR_ALLOC;
  }
  batch->count = count;
  size_t position = 0U;
  for (size_t edge = 0U; edge < store->pair_count * store->action_count; ++edge) {
    if (targets[store->arcs[edge].target_pair]) {
      batch->items[position] = store->arcs[edge];
      ++position;
    }
  }
  free(targets);
  return SG_OK;
}

static sg_status memory_write_records(void *context, const sg_pair_record *records,
                                      size_t record_count) {
  memory_pair_store *store = context;
  for (size_t index = 0U; index < record_count; ++index) {
    if (records[index].pair >= store->pair_count) {
      return SG_ERR_INVALID_ARGUMENT;
    }
    store->records[records[index].pair] = records[index];
  }
  return SG_OK;
}

static void add_keys(sg_automaton_builder *builder, const char *const *states, size_t state_count,
                     const char *const *actions, size_t action_count, const char *const *outputs,
                     size_t output_count) {
  for (size_t index = 0U; index < state_count; ++index) {
    CHECK(sg_automaton_builder_add_state(builder, states[index]) == SG_OK);
  }
  for (size_t index = 0U; index < action_count; ++index) {
    CHECK(sg_automaton_builder_add_action(builder, actions[index]) == SG_OK);
  }
  for (size_t index = 0U; index < output_count; ++index) {
    CHECK(sg_automaton_builder_add_output(builder, outputs[index]) == SG_OK);
  }
}

static void add_cells(sg_automaton_builder *builder, const machine_cell *cells, size_t cell_count) {
  for (size_t index = 0U; index < cell_count; ++index) {
    CHECK(sg_automaton_builder_add_transition(builder, cells[index].source, cells[index].action,
                                              cells[index].target) == SG_OK);
    CHECK(sg_automaton_builder_add_observation(builder, cells[index].source, cells[index].action,
                                               cells[index].output) == SG_OK);
  }
}

static sg_automaton *build_warehouse(void) {
  static const char *const states[] = {
      "west_bay:east", "east_bay:west", "corridor_w:east", "corridor_e:west", "dock:north",
  };
  static const char *const actions[] = {
      "to_corridor",
      "to_wall",
      "go_west",
      "go_east",
  };
  static const char *const outputs[] = {
      "west_landmark",
      "east_landmark",
      "symmetric",
      "dock",
  };
  static const machine_cell cells[] = {
      {"west_bay:east", "to_corridor", "corridor_w:east", "west_landmark"},
      {"west_bay:east", "to_wall", "west_bay:east", "symmetric"},
      {"west_bay:east", "go_west", "west_bay:east", "symmetric"},
      {"west_bay:east", "go_east", "west_bay:east", "symmetric"},
      {"east_bay:west", "to_corridor", "corridor_e:west", "east_landmark"},
      {"east_bay:west", "to_wall", "east_bay:west", "symmetric"},
      {"east_bay:west", "go_west", "east_bay:west", "symmetric"},
      {"east_bay:west", "go_east", "east_bay:west", "symmetric"},
      {"corridor_w:east", "to_corridor", "corridor_w:east", "symmetric"},
      {"corridor_w:east", "to_wall", "west_bay:east", "west_landmark"},
      {"corridor_w:east", "go_west", "dock:north", "dock"},
      {"corridor_w:east", "go_east", "corridor_w:east", "symmetric"},
      {"corridor_e:west", "to_corridor", "corridor_e:west", "symmetric"},
      {"corridor_e:west", "to_wall", "east_bay:west", "east_landmark"},
      {"corridor_e:west", "go_west", "dock:north", "dock"},
      {"corridor_e:west", "go_east", "corridor_e:west", "symmetric"},
      {"dock:north", "to_corridor", "dock:north", "dock"},
      {"dock:north", "to_wall", "dock:north", "dock"},
      {"dock:north", "go_west", "dock:north", "dock"},
      {"dock:north", "go_east", "dock:north", "dock"},
  };

  sg_automaton_builder *builder = NULL;
  CHECK(sg_automaton_builder_init(&builder) == SG_OK);
  add_keys(builder, states, sizeof(states) / sizeof(states[0]), actions,
           sizeof(actions) / sizeof(actions[0]), outputs, sizeof(outputs) / sizeof(outputs[0]));
  add_cells(builder, cells, sizeof(cells) / sizeof(cells[0]));
  sg_automaton *automaton = NULL;
  CHECK(sg_automaton_builder_build(builder, UINT64_C(7), &automaton) == SG_OK);
  sg_automaton_builder_free(builder);
  return automaton;
}

static sg_automaton *build_two_step_observer(void) {
  static const char *const states[] = {"A", "B", "C"};
  static const char *const actions[] = {"ask_a", "ask_b"};
  static const char *const outputs[] = {"yes", "no"};
  static const machine_cell cells[] = {
      {"A", "ask_a", "A", "yes"}, {"A", "ask_b", "A", "no"}, {"B", "ask_a", "B", "no"},
      {"B", "ask_b", "B", "yes"}, {"C", "ask_a", "C", "no"}, {"C", "ask_b", "C", "no"},
  };

  sg_automaton_builder *builder = NULL;
  CHECK(sg_automaton_builder_init(&builder) == SG_OK);
  add_keys(builder, states, sizeof(states) / sizeof(states[0]), actions,
           sizeof(actions) / sizeof(actions[0]), outputs, sizeof(outputs) / sizeof(outputs[0]));
  add_cells(builder, cells, sizeof(cells) / sizeof(cells[0]));
  sg_automaton *automaton = NULL;
  CHECK(sg_automaton_builder_build(builder, UINT64_C(11), &automaton) == SG_OK);
  sg_automaton_builder_free(builder);
  return automaton;
}

static sg_automaton *build_numeric_automaton(const size_t *transitions, const size_t *observations,
                                             uint64_t generation) {
  static const char *const states[] = {"q0", "q1", "q2", "q3", "q4", "q5"};
  static const char *const actions[] = {"a0", "a1", "a2"};
  static const char *const outputs[] = {"o0", "o1"};
  sg_automaton_builder *builder = NULL;
  CHECK(sg_automaton_builder_init(&builder) == SG_OK);
  add_keys(builder, states, NUMERIC_STATE_COUNT, actions, NUMERIC_ACTION_COUNT, outputs,
           NUMERIC_OUTPUT_COUNT);
  for (size_t state = 0U; state < NUMERIC_STATE_COUNT; ++state) {
    for (size_t action = 0U; action < NUMERIC_ACTION_COUNT; ++action) {
      const size_t cell = (state * NUMERIC_ACTION_COUNT) + action;
      CHECK(transitions[cell] < NUMERIC_STATE_COUNT);
      CHECK(observations[cell] < NUMERIC_OUTPUT_COUNT);
      CHECK(sg_automaton_builder_add_transition(builder, states[state], actions[action],
                                                states[transitions[cell]]) == SG_OK);
      CHECK(sg_automaton_builder_add_observation(builder, states[state], actions[action],
                                                 outputs[observations[cell]]) == SG_OK);
    }
  }
  sg_automaton *automaton = NULL;
  CHECK(sg_automaton_builder_build(builder, generation, &automaton) == SG_OK);
  sg_automaton_builder_free(builder);
  return automaton;
}

static void memory_store_init(const sg_pair_oracle *oracle, memory_pair_store *store) {
  store->state_count = NUMERIC_STATE_COUNT;
  store->action_count = NUMERIC_ACTION_COUNT;
  store->pair_count = sg_pair_oracle_pair_count(oracle);
  store->first_states = calloc(store->pair_count, sizeof(*store->first_states));
  store->second_states = calloc(store->pair_count, sizeof(*store->second_states));
  store->records = calloc(store->pair_count, sizeof(*store->records));
  store->arcs = calloc(store->pair_count * store->action_count, sizeof(*store->arcs));
  CHECK(store->first_states != NULL);
  CHECK(store->second_states != NULL);
  CHECK(store->records != NULL);
  CHECK(store->arcs != NULL);
  for (size_t pair = 0U; pair < store->pair_count; ++pair) {
    CHECK(sg_pair_oracle_pair_states(oracle, pair, &store->first_states[pair],
                                     &store->second_states[pair]) == SG_OK);
    CHECK(sg_pair_oracle_record(oracle, pair, &store->records[pair]) == SG_OK);
    for (size_t action = 0U; action < store->action_count; ++action) {
      const size_t edge = (pair * store->action_count) + action;
      store->arcs[edge].source_pair = pair;
      store->arcs[edge].action = action;
      CHECK(sg_pair_oracle_pair_step(oracle, pair, action, &store->arcs[edge].target_pair,
                                     &store->arcs[edge].outputs_differ) == SG_OK);
    }
  }
}

static void memory_store_free(memory_pair_store *store) {
  free(store->first_states);
  free(store->second_states);
  free(store->records);
  free(store->arcs);
  *store = (memory_pair_store){0};
}

static void check_pair_records_equal(const sg_pair_record *first, const sg_pair_record *second) {
  CHECK(first->pair == second->pair);
  CHECK(first->mergeable == second->mergeable);
  CHECK(first->merge_distance == second->merge_distance);
  CHECK(first->merge_action == second->merge_action);
  CHECK(first->merge_next_pair == second->merge_next_pair);
  CHECK(first->merge_support_count == second->merge_support_count);
  CHECK(first->resolvable == second->resolvable);
  CHECK(first->resolution_distance == second->resolution_distance);
  CHECK(first->resolution_action == second->resolution_action);
  CHECK(first->resolution_next_pair == second->resolution_next_pair);
  CHECK(first->resolution_support_count == second->resolution_support_count);
}

static size_t state_id(const sg_automaton *automaton, const char *key) {
  size_t state = SG_INDEX_NONE;
  CHECK(sg_automaton_find_state(automaton, key, &state) == SG_OK);
  return state;
}

static size_t action_id(const sg_automaton *automaton, const char *key) {
  size_t action = SG_INDEX_NONE;
  CHECK(sg_automaton_find_action(automaton, key, &action) == SG_OK);
  return action;
}

static sg_pair_oracle *restore_oracle(const sg_automaton *automaton, const sg_pair_oracle *source) {
  const size_t pair_count = sg_pair_oracle_pair_count(source);
  sg_pair_record *records = calloc(pair_count, sizeof(*records));
  CHECK(records != NULL);
  for (size_t pair = 0U; pair < pair_count; ++pair) {
    CHECK(sg_pair_oracle_record(source, pair, &records[pair]) == SG_OK);
  }
  sg_pair_oracle *restored = NULL;
  CHECK(sg_pair_oracle_restore(automaton, records, pair_count, &restored) == SG_OK);
  free(records);
  return restored;
}

static void check_plan_semantics_equal(const sg_plan_result *first, const sg_plan_result *second) {
  CHECK(first->outcome == second->outcome);
  CHECK(first->method == second->method);
  CHECK(first->word.length == second->word.length);
  for (size_t index = 0U; index < first->word.length; ++index) {
    CHECK(first->word.actions[index] == second->word.actions[index]);
  }
  CHECK(first->final_state == second->final_state);
  CHECK(first->final_support_size == second->final_support_size);
  CHECK(first->best_support_size == second->best_support_size);
  CHECK(first->worst_support_size == second->worst_support_size);
  CHECK(first->branch_count == second->branch_count);
  CHECK(first->expansions == second->expansions);
  CHECK(first->homing == second->homing);
  CHECK(first->generation == second->generation);
}

static sg_status count_explanation(void *context, size_t step, size_t action,
                                   const size_t *predicted_states, size_t predicted_count,
                                   const size_t *output_trace, size_t trace_length,
                                   const size_t *branch_states, size_t branch_count) {
  explain_counts *counts = context;
  CHECK(counts != NULL);
  CHECK(predicted_states != NULL);
  CHECK(predicted_count != 0U);
  CHECK(branch_states != NULL);
  CHECK(branch_count != 0U);
  CHECK(output_trace != NULL || trace_length == 0U);
  ++counts->calls;
  if (step == 0U) {
    CHECK(action == SG_INDEX_NONE);
    CHECK(trace_length == 0U);
    ++counts->initial_rows;
  } else {
    CHECK(action != SG_INDEX_NONE);
    CHECK(trace_length == step);
    ++counts->action_rows;
  }
  if (branch_count == 1U) {
    ++counts->singleton_rows;
  }
  return SG_OK;
}

static void test_names_and_builder_validation(void) {
  CHECK(strcmp(sg_status_name(SG_ERR_STALE_GENERATION), "STALE_GENERATION") == 0);
  CHECK(strcmp(sg_plan_outcome_name(SG_OUTCOME_RESOURCE_BOUND), "RESOURCE_BOUND") == 0);
  CHECK(strcmp(sg_plan_method_name(SG_METHOD_PARTITION_BFS), "PARTITION_BFS") == 0);
  CHECK(strcmp(sg_monitor_decision_name(SG_MONITOR_MODEL_VIOLATION), "MODEL_VIOLATION") == 0);

  sg_automaton_builder *builder = NULL;
  CHECK(sg_automaton_builder_init(&builder) == SG_OK);
  CHECK(sg_automaton_builder_add_state(builder, "S") == SG_OK);
  CHECK(sg_automaton_builder_add_state(builder, "T") == SG_OK);
  CHECK(sg_automaton_builder_add_state(builder, "S") == SG_ERR_DUPLICATE);
  CHECK(sg_automaton_builder_add_action(builder, "a") == SG_OK);
  CHECK(sg_automaton_builder_add_output(builder, "quiet") == SG_OK);
  CHECK(sg_automaton_builder_add_transition(builder, "S", "a", "T") == SG_OK);
  CHECK(sg_automaton_builder_add_observation(builder, "S", "a", "quiet") == SG_OK);
  sg_automaton *automaton = NULL;
  CHECK(sg_automaton_builder_build(builder, 1U, &automaton) == SG_ERR_INCOMPLETE);
  CHECK(automaton == NULL);
  CHECK(sg_automaton_builder_add_transition(builder, "T", "a", "T") == SG_OK);
  CHECK(sg_automaton_builder_add_transition(builder, "T", "a", "S") == SG_OK);
  CHECK(sg_automaton_builder_add_observation(builder, "T", "a", "quiet") == SG_OK);
  CHECK(sg_automaton_builder_build(builder, 1U, &automaton) == SG_ERR_NONDETERMINISTIC);
  sg_automaton_builder_free(builder);
}

static void test_automaton_and_oracle(void) {
  sg_automaton *automaton = build_warehouse();
  CHECK(sg_automaton_generation(automaton) == 7U);
  CHECK(sg_automaton_state_count(automaton) == 5U);
  CHECK(sg_automaton_action_count(automaton) == 4U);
  CHECK(sg_automaton_output_count(automaton) == 4U);
  CHECK(sg_automaton_transition_count(automaton) == 20U);
  CHECK(sg_automaton_find_state(automaton, "missing", &(size_t){0U}) == SG_ERR_NOT_FOUND);

  sg_pair_oracle *oracle = NULL;
  CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
  CHECK(sg_pair_oracle_pair_count(oracle) == 15U);
  CHECK(sg_pair_oracle_pair_edge_count(oracle) == 60U);
  CHECK(sg_pair_oracle_mergeable_pair_count(oracle) == 15U);
  CHECK(sg_pair_oracle_resolvable_pair_count(oracle) == 15U);

  const size_t west = state_id(automaton, "west_bay:east");
  const size_t east = state_id(automaton, "east_bay:west");
  const size_t to_corridor = action_id(automaton, "to_corridor");
  size_t west_east_pair = SG_INDEX_NONE;
  for (size_t pair = 0U; pair < sg_pair_oracle_pair_count(oracle); ++pair) {
    size_t first = 0U;
    size_t second = 0U;
    CHECK(sg_pair_oracle_pair_states(oracle, pair, &first, &second) == SG_OK);
    if (first == west && second == east) {
      west_east_pair = pair;
    }
  }
  CHECK(west_east_pair != SG_INDEX_NONE);
  bool outputs_differ = false;
  size_t next_pair = SG_INDEX_NONE;
  CHECK(sg_pair_oracle_pair_step(oracle, west_east_pair, to_corridor, &next_pair,
                                 &outputs_differ) == SG_OK);
  CHECK(outputs_differ);
  CHECK(next_pair != SG_INDEX_NONE);

  sg_word merge = {0};
  CHECK(sg_pair_oracle_merge_word(oracle, west, east, &merge) == SG_OK);
  CHECK(merge.length == 2U);
  CHECK(strcmp(sg_automaton_action_key(automaton, merge.actions[0]), "to_corridor") == 0);
  CHECK(strcmp(sg_automaton_action_key(automaton, merge.actions[1]), "go_west") == 0);
  sg_word_free(&merge);

  sg_word resolution = {0};
  CHECK(sg_pair_oracle_resolution_word(oracle, west, east, &resolution) == SG_OK);
  CHECK(resolution.length == 1U);
  CHECK(resolution.actions[0] == to_corridor);
  sg_word_free(&resolution);

  const size_t pair_count = sg_pair_oracle_pair_count(oracle);
  sg_pair_record *records = calloc(pair_count, sizeof(*records));
  CHECK(records != NULL);
  for (size_t pair = 0U; pair < pair_count; ++pair) {
    CHECK(sg_pair_oracle_record(oracle, pair, &records[pair]) == SG_OK);
  }
  sg_pair_oracle *restored = NULL;
  CHECK(sg_pair_oracle_restore(automaton, records, pair_count, &restored) == SG_OK);
  CHECK(sg_pair_oracle_mergeable_pair_count(restored) == pair_count);
  sg_pair_oracle_free(restored);
  records[west_east_pair].resolution_action = SG_INDEX_NONE;
  CHECK(sg_pair_oracle_restore(automaton, records, pair_count, &restored) == SG_ERR_INVALID_MODEL);
  free(records);
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);
}

static void test_planners_explanation_and_monitor(void) {
  sg_automaton *automaton = build_warehouse();
  sg_pair_oracle *oracle = NULL;
  CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
  const size_t initial[] = {
      state_id(automaton, "west_bay:east"),
      state_id(automaton, "east_bay:west"),
  };

  sg_plan_result sync = {0};
  CHECK(sg_plan_sync(automaton, oracle, initial, 2U, 16U, &sync) == SG_OK);
  CHECK(sync.outcome == SG_OUTCOME_PLAN);
  CHECK(sync.method == SG_METHOD_PAIR_MERGE);
  CHECK(sync.word.length == 2U);
  CHECK(sync.final_state == state_id(automaton, "dock:north"));
  CHECK(sync.final_support_size == 1U);
  CHECK(sync.generation == 7U);

  size_t final_states[sizeof(initial) / sizeof(initial[0])] = {0U};
  size_t final_count = 0U;
  CHECK(sg_apply_word(automaton, initial, 2U, &sync.word, final_states, &final_count) == SG_OK);
  CHECK(final_count == 1U);
  CHECK(final_states[0] == sync.final_state);

  explain_counts explanation = {0};
  CHECK(sg_explain_plan(automaton, sync.generation, initial, 2U, &sync.word, count_explanation,
                        &explanation) == SG_OK);
  CHECK(explanation.calls == 5U);
  CHECK(explanation.initial_rows == 1U);
  CHECK(explanation.action_rows == 4U);
  CHECK(explanation.singleton_rows == 4U);
  CHECK(sg_explain_plan(automaton, sync.generation + 1U, initial, 2U, &sync.word, count_explanation,
                        &explanation) == SG_ERR_STALE_GENERATION);

  const size_t corridor[] = {
      state_id(automaton, "corridor_w:east"),
      state_id(automaton, "corridor_e:west"),
  };
  sg_monitor_result monitor = {0};
  CHECK(sg_validate_update(automaton, sync.generation, initial, 2U, &sync.word, 1U, corridor, 2U,
                           true, &monitor) == SG_OK);
  CHECK(monitor.decision == SG_MONITOR_CONTINUE);
  CHECK(monitor.expected_count == 2U);
  sg_monitor_result_free(&monitor);
  CHECK(sg_validate_update(automaton, sync.generation, initial, 2U, &sync.word, 1U, corridor, 1U,
                           true, &monitor) == SG_OK);
  CHECK(monitor.decision == SG_MONITOR_REPLAN);
  sg_monitor_result_free(&monitor);
  const size_t dock[] = {state_id(automaton, "dock:north")};
  CHECK(sg_validate_update(automaton, sync.generation, initial, 2U, &sync.word, 1U, dock, 1U, true,
                           &monitor) == SG_OK);
  CHECK(monitor.decision == SG_MONITOR_MODEL_VIOLATION);
  CHECK(monitor.unexpected_count == 1U);
  sg_monitor_result_free(&monitor);
  CHECK(sg_validate_update(automaton, sync.generation, initial, 2U, &sync.word, 1U, NULL, 0U, false,
                           &monitor) == SG_OK);
  CHECK(monitor.decision == SG_MONITOR_WAIT);
  sg_monitor_result_free(&monitor);
  CHECK(sg_validate_update(automaton, sync.generation + 1U, initial, 2U, &sync.word, 1U, corridor,
                           2U, true, &monitor) == SG_OK);
  CHECK(monitor.decision == SG_MONITOR_STALE_GENERATION);
  sg_monitor_result_free(&monitor);

  sg_plan_result disambiguation = {0};
  CHECK(sg_plan_disambiguate(automaton, oracle, initial, 2U, 1U, 16U, &disambiguation) == SG_OK);
  CHECK(disambiguation.outcome == SG_OUTCOME_PLAN);
  CHECK(disambiguation.method == SG_METHOD_PAIR_RESOLUTION);
  CHECK(disambiguation.word.length == 1U);
  CHECK(disambiguation.word.actions[0] == action_id(automaton, "to_corridor"));
  CHECK(disambiguation.branch_count == 2U);
  CHECK(disambiguation.worst_support_size == 1U);
  CHECK(disambiguation.homing);

  sg_plan_result_free(&disambiguation);
  sg_plan_result_free(&sync);
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);
}

static void test_exact_partition_search(void) {
  sg_automaton *automaton = build_two_step_observer();
  sg_pair_oracle *oracle = NULL;
  CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
  const size_t initial[] = {
      state_id(automaton, "A"),
      state_id(automaton, "B"),
      state_id(automaton, "C"),
  };

  sg_plan_result result = {0};
  CHECK(sg_plan_disambiguate(automaton, oracle, initial, 3U, 1U, 1U, &result) == SG_OK);
  CHECK(result.outcome == SG_OUTCOME_RESOURCE_BOUND);
  sg_plan_result_free(&result);

  CHECK(sg_plan_disambiguate(automaton, oracle, initial, 3U, 1U, 16U, &result) == SG_OK);
  CHECK(result.outcome == SG_OUTCOME_PLAN);
  CHECK(result.method == SG_METHOD_PARTITION_BFS);
  CHECK(result.word.length == 2U);
  CHECK(result.worst_support_size == 1U);
  CHECK(result.branch_count == 3U);
  sg_plan_result_free(&result);

  CHECK(sg_plan_sync(automaton, oracle, initial, 3U, 16U, &result) == SG_OK);
  CHECK(result.outcome == SG_OUTCOME_NO_PLAN);
  sg_plan_result_free(&result);
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);
}

static void test_oracle_source_equivalence(void) {
  sg_automaton *warehouse = build_warehouse();
  sg_pair_oracle *built = NULL;
  CHECK(sg_pair_oracle_build(warehouse, &built) == SG_OK);
  sg_pair_oracle *restored = restore_oracle(warehouse, built);
  const size_t ambiguous[] = {
      state_id(warehouse, "west_bay:east"),
      state_id(warehouse, "east_bay:west"),
  };
  const size_t singleton[] = {state_id(warehouse, "dock:north")};
  sg_plan_result first = {0};
  sg_plan_result second = {0};

  CHECK(sg_plan_sync(warehouse, built, ambiguous, 2U, 16U, &first) == SG_OK);
  CHECK(sg_plan_sync(warehouse, restored, ambiguous, 2U, 16U, &second) == SG_OK);
  check_plan_semantics_equal(&first, &second);
  sg_plan_result_free(&first);
  sg_plan_result_free(&second);

  CHECK(sg_plan_disambiguate(warehouse, built, ambiguous, 2U, 1U, 16U, &first) == SG_OK);
  CHECK(sg_plan_disambiguate(warehouse, restored, ambiguous, 2U, 1U, 16U, &second) == SG_OK);
  check_plan_semantics_equal(&first, &second);
  sg_plan_result_free(&first);
  sg_plan_result_free(&second);

  CHECK(sg_plan_sync(warehouse, built, singleton, 1U, 16U, &first) == SG_OK);
  CHECK(sg_plan_sync(warehouse, restored, singleton, 1U, 16U, &second) == SG_OK);
  check_plan_semantics_equal(&first, &second);
  CHECK(first.outcome == SG_OUTCOME_ALREADY_SATISFIED);
  sg_plan_result_free(&first);
  sg_plan_result_free(&second);

  sg_pair_oracle_free(restored);
  sg_pair_oracle_free(built);
  sg_automaton_free(warehouse);

  sg_automaton *observer = build_two_step_observer();
  CHECK(sg_pair_oracle_build(observer, &built) == SG_OK);
  restored = restore_oracle(observer, built);
  const size_t observer_initial[] = {
      state_id(observer, "A"),
      state_id(observer, "B"),
      state_id(observer, "C"),
  };

  CHECK(sg_plan_disambiguate(observer, built, observer_initial, 3U, 1U, 1U, &first) == SG_OK);
  CHECK(sg_plan_disambiguate(observer, restored, observer_initial, 3U, 1U, 1U, &second) == SG_OK);
  check_plan_semantics_equal(&first, &second);
  CHECK(first.outcome == SG_OUTCOME_RESOURCE_BOUND);
  sg_plan_result_free(&first);
  sg_plan_result_free(&second);

  CHECK(sg_plan_disambiguate(observer, built, observer_initial, 3U, 1U, 16U, &first) == SG_OK);
  CHECK(sg_plan_disambiguate(observer, restored, observer_initial, 3U, 1U, 16U, &second) == SG_OK);
  check_plan_semantics_equal(&first, &second);
  CHECK(first.method == SG_METHOD_PARTITION_BFS);
  sg_plan_result_free(&first);
  sg_plan_result_free(&second);

  CHECK(sg_plan_sync(observer, built, observer_initial, 3U, 16U, &first) == SG_OK);
  CHECK(sg_plan_sync(observer, restored, observer_initial, 3U, 16U, &second) == SG_OK);
  check_plan_semantics_equal(&first, &second);
  CHECK(first.outcome == SG_OUTCOME_NO_PLAN);
  sg_plan_result_free(&first);
  sg_plan_result_free(&second);

  sg_pair_oracle_free(restored);
  sg_pair_oracle_free(built);
  sg_automaton_free(observer);
}

static void test_incremental_pair_maintenance(void) {
  size_t transitions[NUMERIC_CELL_COUNT] = {0};
  size_t observations[NUMERIC_CELL_COUNT] = {0};
  for (size_t state = 0U; state < NUMERIC_STATE_COUNT; ++state) {
    for (size_t action = 0U; action < NUMERIC_ACTION_COUNT; ++action) {
      const size_t cell = (state * NUMERIC_ACTION_COUNT) + action;
      transitions[cell] = (state + action + 1U) % NUMERIC_STATE_COUNT;
      observations[cell] = (state + action) % NUMERIC_OUTPUT_COUNT;
    }
  }
  sg_automaton *automaton = build_numeric_automaton(transitions, observations, UINT64_C(1));
  sg_pair_oracle *oracle = NULL;
  CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
  memory_pair_store memory = {0};
  memory_store_init(oracle, &memory);
  sg_pair_store store = {
      .context = &memory,
      .state_count = memory.state_count,
      .action_count = memory.action_count,
      .pair_count = memory.pair_count,
      .read_records = memory_read_records,
      .read_outgoing = memory_read_outgoing,
      .read_incoming = memory_read_incoming,
      .write_records = memory_write_records,
  };
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);

  for (size_t step = 0U; step < NUMERIC_MUTATION_COUNT; ++step) {
    const size_t state = ((step * NUMERIC_MUTATION_STRIDE) + 1U) % NUMERIC_STATE_COUNT;
    const size_t action = ((step * NUMERIC_ACTION_STRIDE) + 2U) % NUMERIC_ACTION_COUNT;
    const size_t cell = (state * NUMERIC_ACTION_COUNT) + action;
    if (step % NUMERIC_ACTION_COUNT != 0U) {
      transitions[cell] =
          (transitions[cell] + 1U + (step % NUMERIC_MUTATION_STRIDE)) % NUMERIC_STATE_COUNT;
    }
    if (step % NUMERIC_ACTION_COUNT != 1U) {
      observations[cell] ^= 1U;
    }
    automaton = build_numeric_automaton(transitions, observations, (uint64_t)step + UINT64_C(2));
    CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
    size_t seeds[NUMERIC_STATE_COUNT] = {0};
    size_t seed_count = 0U;
    for (size_t pair = 0U; pair < memory.pair_count; ++pair) {
      if (memory.first_states[pair] == state || memory.second_states[pair] == state) {
        seeds[seed_count] = pair;
        ++seed_count;
        const size_t edge = (pair * memory.action_count) + action;
        CHECK(sg_pair_oracle_pair_step(oracle, pair, action, &memory.arcs[edge].target_pair,
                                       &memory.arcs[edge].outputs_differ) == SG_OK);
      }
    }
    CHECK(seed_count == NUMERIC_STATE_COUNT);
    sg_pair_repair_metrics metrics = {0};
    CHECK(sg_pair_store_repair(&store, seeds, seed_count, memory.pair_count, &metrics) == SG_OK);
    CHECK(metrics.pair_records_touched >= seed_count);
    CHECK(metrics.pair_records_touched <= memory.pair_count);
    for (size_t pair = 0U; pair < memory.pair_count; ++pair) {
      sg_pair_record expected = {0};
      CHECK(sg_pair_oracle_record(oracle, pair, &expected) == SG_OK);
      check_pair_records_equal(&memory.records[pair], &expected);
    }
    sg_pair_oracle_free(oracle);
    sg_automaton_free(automaton);
  }

  automaton = build_numeric_automaton(transitions, observations, UINT64_C(50));
  CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
  const sg_pair_record_source source = {
      .context = &memory,
      .read = memory_read_records,
  };
  const size_t hypotheses[] = {0U, 1U, 2U, 3U};
  sg_plan_result expected_plan = {0};
  sg_plan_result actual_plan = {0};
  CHECK(sg_plan_sync(automaton, oracle, hypotheses, 4U, 64U, &expected_plan) == SG_OK);
  CHECK(sg_plan_sync_from_records(automaton, &source, hypotheses, 4U, 64U, &actual_plan) == SG_OK);
  check_plan_semantics_equal(&expected_plan, &actual_plan);
  sg_plan_result_free(&expected_plan);
  sg_plan_result_free(&actual_plan);
  CHECK(sg_plan_disambiguate(automaton, oracle, hypotheses, 4U, 1U, 64U, &expected_plan) == SG_OK);
  CHECK(sg_plan_disambiguate_from_records(automaton, &source, hypotheses, 4U, 1U, 64U,
                                          &actual_plan) == SG_OK);
  check_plan_semantics_equal(&expected_plan, &actual_plan);
  sg_plan_result_free(&expected_plan);
  sg_plan_result_free(&actual_plan);
  CHECK(memory.record_reads != 0U);

  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);
  memory_store_free(&memory);
}

int main(void) {
  test_names_and_builder_validation();
  test_automaton_and_oracle();
  test_planners_explanation_and_monitor();
  test_exact_partition_search();
  test_oracle_source_equivalence();
  test_incremental_pair_maintenance();
  return EXIT_SUCCESS;
}
