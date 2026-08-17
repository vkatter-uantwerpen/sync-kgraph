#include "sync_kgraph/sync.h"

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

int main(void) {
  test_names_and_builder_validation();
  test_automaton_and_oracle();
  test_planners_explanation_and_monitor();
  test_exact_partition_search();
  return EXIT_SUCCESS;
}
