#ifndef SYNC_KGRAPH_SYNC_H
#define SYNC_KGRAPH_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SG_INDEX_NONE SIZE_MAX

typedef enum {
  SG_OK = 0,
  SG_ERR_ALLOC,
  SG_ERR_INVALID_ARGUMENT,
  SG_ERR_DUPLICATE,
  SG_ERR_NOT_FOUND,
  SG_ERR_INCOMPLETE,
  SG_ERR_NONDETERMINISTIC,
  SG_ERR_INVALID_MODEL,
  SG_ERR_RESOURCE_BOUND,
  SG_ERR_STALE_GENERATION,
} sg_status;

typedef enum {
  SG_OUTCOME_PLAN = 0,
  SG_OUTCOME_ALREADY_SATISFIED,
  SG_OUTCOME_NO_PLAN,
  SG_OUTCOME_RESOURCE_BOUND,
} sg_plan_outcome;

typedef enum {
  SG_METHOD_NONE = 0,
  SG_METHOD_PAIR_MERGE,
  SG_METHOD_PAIR_RESOLUTION,
  SG_METHOD_PARTITION_BFS,
  SG_METHOD_BELIEF_BFS,
} sg_plan_method;

typedef enum {
  SG_MONITOR_CONTINUE = 0,
  SG_MONITOR_REPLAN,
  SG_MONITOR_MODEL_VIOLATION,
  SG_MONITOR_STALE_GENERATION,
  SG_MONITOR_WAIT,
} sg_monitor_decision;

typedef struct sg_automaton sg_automaton;
typedef struct sg_automaton_builder sg_automaton_builder;
typedef struct sg_pair_oracle sg_pair_oracle;

typedef struct {
  size_t *actions;
  size_t length;
  size_t capacity;
} sg_word;

typedef struct {
  sg_plan_outcome outcome;
  sg_plan_method method;
  sg_word word;
  size_t final_state;
  size_t final_support_size;
  size_t best_support_size;
  size_t worst_support_size;
  size_t branch_count;
  size_t expansions;
  bool homing;
  uint64_t generation;
  uint64_t planning_time_us;
} sg_plan_result;

typedef struct {
  size_t pair;
  bool mergeable;
  size_t merge_distance;
  size_t merge_action;
  size_t merge_next_pair;
  bool resolvable;
  size_t resolution_distance;
  size_t resolution_action;
  size_t resolution_next_pair;
} sg_pair_record;

typedef struct {
  sg_monitor_decision decision;
  size_t *expected_states;
  size_t expected_count;
  size_t *unexpected_states;
  size_t unexpected_count;
  uint64_t generation;
} sg_monitor_result;

typedef sg_status (*sg_explain_visitor)(void *context, size_t step, size_t action,
                                        const size_t *predicted_states, size_t predicted_count,
                                        const size_t *output_trace, size_t trace_length,
                                        const size_t *branch_states, size_t branch_count);

const char *sg_status_name(sg_status status);
const char *sg_plan_outcome_name(sg_plan_outcome outcome);
const char *sg_plan_method_name(sg_plan_method method);
const char *sg_monitor_decision_name(sg_monitor_decision decision);

sg_status sg_word_init(sg_word *word);
void sg_word_free(sg_word *word);
sg_status sg_word_append(sg_word *word, size_t action);

sg_status sg_automaton_builder_init(sg_automaton_builder **builder);
void sg_automaton_builder_free(sg_automaton_builder *builder);
sg_status sg_automaton_builder_add_state(sg_automaton_builder *builder, const char *state_key);
sg_status sg_automaton_builder_add_action(sg_automaton_builder *builder, const char *action_key);
sg_status sg_automaton_builder_add_output(sg_automaton_builder *builder, const char *output_key);
sg_status sg_automaton_builder_add_transition(sg_automaton_builder *builder, const char *source_key,
                                              const char *action_key, const char *target_key);
sg_status sg_automaton_builder_add_observation(sg_automaton_builder *builder,
                                               const char *source_key, const char *action_key,
                                               const char *output_key);
sg_status sg_automaton_builder_build(sg_automaton_builder *builder, uint64_t generation,
                                     sg_automaton **automaton);

void sg_automaton_free(sg_automaton *automaton);
uint64_t sg_automaton_generation(const sg_automaton *automaton);
size_t sg_automaton_state_count(const sg_automaton *automaton);
size_t sg_automaton_action_count(const sg_automaton *automaton);
size_t sg_automaton_output_count(const sg_automaton *automaton);
size_t sg_automaton_transition_count(const sg_automaton *automaton);
const char *sg_automaton_state_key(const sg_automaton *automaton, size_t state);
const char *sg_automaton_action_key(const sg_automaton *automaton, size_t action);
const char *sg_automaton_output_key(const sg_automaton *automaton, size_t output);
size_t sg_automaton_transition(const sg_automaton *automaton, size_t state, size_t action);
size_t sg_automaton_observation(const sg_automaton *automaton, size_t state, size_t action);
sg_status sg_automaton_find_state(const sg_automaton *automaton, const char *state_key,
                                  size_t *state);
sg_status sg_automaton_find_action(const sg_automaton *automaton, const char *action_key,
                                   size_t *action);
sg_status sg_automaton_find_output(const sg_automaton *automaton, const char *output_key,
                                   size_t *output);

sg_status sg_pair_oracle_build(const sg_automaton *automaton, sg_pair_oracle **oracle);
sg_status sg_pair_oracle_restore(const sg_automaton *automaton, const sg_pair_record *records,
                                 size_t record_count, sg_pair_oracle **oracle);
void sg_pair_oracle_free(sg_pair_oracle *oracle);
size_t sg_pair_oracle_pair_count(const sg_pair_oracle *oracle);
size_t sg_pair_oracle_pair_edge_count(const sg_pair_oracle *oracle);
size_t sg_pair_oracle_mergeable_pair_count(const sg_pair_oracle *oracle);
size_t sg_pair_oracle_resolvable_pair_count(const sg_pair_oracle *oracle);
sg_status sg_pair_oracle_pair_states(const sg_pair_oracle *oracle, size_t pair, size_t *first,
                                     size_t *second);
sg_status sg_pair_oracle_pair_step(const sg_pair_oracle *oracle, size_t pair, size_t action,
                                   size_t *next_pair, bool *outputs_differ);
sg_status sg_pair_oracle_record(const sg_pair_oracle *oracle, size_t pair, sg_pair_record *record);
sg_status sg_pair_oracle_merge_word(const sg_pair_oracle *oracle, size_t first, size_t second,
                                    sg_word *word);
sg_status sg_pair_oracle_resolution_word(const sg_pair_oracle *oracle, size_t first, size_t second,
                                         sg_word *word);

sg_status sg_plan_sync(const sg_automaton *automaton, const sg_pair_oracle *oracle,
                       const size_t *initial_states, size_t initial_count, size_t budget,
                       sg_plan_result *result);
sg_status sg_plan_sync_allowed(const sg_automaton *automaton, const sg_pair_oracle *oracle,
                               const size_t *initial_states, size_t initial_count,
                               const size_t *allowed_actions, size_t allowed_action_count,
                               size_t budget, sg_plan_result *result);
sg_status sg_plan_disambiguate(const sg_automaton *automaton, const sg_pair_oracle *oracle,
                               const size_t *initial_states, size_t initial_count, size_t bound,
                               const size_t *allowed_actions, size_t allowed_action_count,
                               size_t budget, sg_plan_result *result);
sg_status sg_plan_goal(const sg_automaton *automaton, const size_t *initial_states,
                       size_t initial_count, const size_t *goal_states, size_t goal_count,
                       const size_t *allowed_actions, size_t allowed_action_count, size_t budget,
                       sg_plan_result *result);
void sg_plan_result_free(sg_plan_result *result);

sg_status sg_apply_word(const sg_automaton *automaton, const size_t *initial_states,
                        size_t initial_count, const sg_word *word, size_t *output_states,
                        size_t *output_count);
sg_status sg_explain_plan(const sg_automaton *automaton, uint64_t plan_generation,
                          const size_t *initial_states, size_t initial_count, const sg_word *word,
                          sg_explain_visitor visitor, void *context);
sg_status sg_validate_update(const sg_automaton *automaton, uint64_t plan_generation,
                             const size_t *initial_states, size_t initial_count,
                             const sg_word *word, size_t completed_steps,
                             const size_t *reported_states, size_t reported_count,
                             bool localizer_available, sg_monitor_result *result);
void sg_monitor_result_free(sg_monitor_result *result);

#ifdef __cplusplus
}
#endif

#endif
