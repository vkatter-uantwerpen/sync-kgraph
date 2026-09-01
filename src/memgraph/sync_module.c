#include "sync_kgraph/sync.h"

#include "mg_procedure.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SG_MATERIALIZE_BATCH 512U
#define SG_ERROR_MESSAGE_CAPACITY 192U
#define SG_VALIDATE_REPORTED_ARGUMENT 5U
#define SG_VALIDATE_AVAILABLE_ARGUMENT 6U

// NOLINTNEXTLINE(misc-use-internal-linkage)
int mgp_init_module(struct mgp_module *module, struct mgp_memory *memory);
// NOLINTNEXTLINE(misc-use-internal-linkage)
int mgp_shutdown_module(void);

typedef struct {
  uint64_t generation;
  bool dirty;
  bool prepared;
} model_metadata;

typedef enum {
  DOMAIN_STATE = 0,
  DOMAIN_ACTION,
  DOMAIN_OUTPUT,
} domain_kind;

typedef struct {
  struct mgp_result *result;
  const sg_automaton *automaton;
  struct mgp_memory *memory;
} explain_context;

static bool mg_ok(enum mgp_error error) {
  return error == MGP_ERROR_NO_ERROR;
}

#ifdef SYNC_KGRAPH_MGP_COMPAT
static enum mgp_error sync_compat_list_append_move(struct mgp_list *list, struct mgp_value *value) {
  const enum mgp_error error = mgp_list_append(list, value);
  if (error == MGP_ERROR_NO_ERROR) {
    mgp_value_destroy(value);
  }
  return error;
}

static enum mgp_error sync_compat_map_insert_move(struct mgp_map *map, const char *key,
                                                  struct mgp_value *value) {
  const enum mgp_error error = mgp_map_insert(map, key, value);
  if (error == MGP_ERROR_NO_ERROR) {
    mgp_value_destroy(value);
  }
  return error;
}

#define mgp_list_append_move sync_compat_list_append_move
#define mgp_map_insert_move sync_compat_map_insert_move
#define mgp_unordered_map_make_empty mgp_map_make_empty
#endif

static void set_error(struct mgp_result *result, const char *message) {
  (void)mgp_result_set_error_msg(result, message);
}

static void set_status_error(struct mgp_result *result, const char *operation, sg_status status) {
  char message[SG_ERROR_MESSAGE_CAPACITY] = {0};
  (void)snprintf(message, sizeof(message), "%s: %s", operation, sg_status_name(status));
  set_error(result, message);
}

static bool insert_value(struct mgp_result_record *record, const char *field,
                         struct mgp_value *value) {
  if (value == NULL) {
    return false;
  }
  const bool ok = mg_ok(mgp_result_record_insert(record, field, value));
  mgp_value_destroy(value);
  return ok;
}

static bool insert_string(struct mgp_result_record *record, const char *field, const char *value,
                          struct mgp_memory *memory) {
  struct mgp_value *created = NULL;
  return mg_ok(mgp_value_make_string(value, memory, &created)) &&
         insert_value(record, field, created);
}

static bool insert_int(struct mgp_result_record *record, const char *field, int64_t value,
                       struct mgp_memory *memory) {
  struct mgp_value *created = NULL;
  return mg_ok(mgp_value_make_int(value, memory, &created)) && insert_value(record, field, created);
}

static bool insert_bool(struct mgp_result_record *record, const char *field, bool value,
                        struct mgp_memory *memory) {
  struct mgp_value *created = NULL;
  return mg_ok(mgp_value_make_bool(value ? 1 : 0, memory, &created)) &&
         insert_value(record, field, created);
}

static bool new_record(struct mgp_result *result, struct mgp_result_record **record) {
  return mg_ok(mgp_result_new_record(result, record)) && *record != NULL;
}

static bool get_arg(struct mgp_list *arguments, size_t index, struct mgp_value **value) {
  size_t count = 0U;
  return mg_ok(mgp_list_size(arguments, &count)) && index < count &&
         mg_ok(mgp_list_at(arguments, index, value)) && *value != NULL;
}

static bool get_string_arg(struct mgp_list *arguments, size_t index, const char **value) {
  struct mgp_value *argument = NULL;
  return get_arg(arguments, index, &argument) && mg_ok(mgp_value_get_string(argument, value)) &&
         *value != NULL;
}

static bool get_int_arg(struct mgp_list *arguments, size_t index, int64_t *value) {
  struct mgp_value *argument = NULL;
  return get_arg(arguments, index, &argument) && mg_ok(mgp_value_get_int(argument, value));
}

static bool get_bool_arg_default(struct mgp_list *arguments, size_t index, bool default_value,
                                 bool *value) {
  struct mgp_value *argument = NULL;
  if (!get_arg(arguments, index, &argument)) {
    *value = default_value;
    return true;
  }
  enum mgp_value_type type = MGP_VALUE_TYPE_NULL;
  if (!mg_ok(mgp_value_get_type(argument, &type))) {
    return false;
  }
  if (type == MGP_VALUE_TYPE_NULL) {
    *value = default_value;
    return true;
  }
  int raw = 0;
  if (type != MGP_VALUE_TYPE_BOOL || !mg_ok(mgp_value_get_bool(argument, &raw))) {
    return false;
  }
  *value = raw != 0;
  return true;
}

static bool params_insert_value(struct mgp_map *params, const char *key, struct mgp_value *value) {
  if (params == NULL || value == NULL) {
    if (value != NULL) {
      mgp_value_destroy(value);
    }
    return false;
  }
  if (!mg_ok(mgp_map_insert_move(params, key, value))) {
    mgp_value_destroy(value);
    return false;
  }
  return true;
}

static bool params_insert_string(struct mgp_map *params, const char *key, const char *value,
                                 struct mgp_memory *memory) {
  struct mgp_value *created = NULL;
  return mg_ok(mgp_value_make_string(value, memory, &created)) &&
         params_insert_value(params, key, created);
}

static bool params_insert_int(struct mgp_map *params, const char *key, int64_t value,
                              struct mgp_memory *memory) {
  struct mgp_value *created = NULL;
  return mg_ok(mgp_value_make_int(value, memory, &created)) &&
         params_insert_value(params, key, created);
}

static bool params_insert_bool(struct mgp_map *params, const char *key, bool value,
                               struct mgp_memory *memory) {
  struct mgp_value *created = NULL;
  return mg_ok(mgp_value_make_bool(value ? 1 : 0, memory, &created)) &&
         params_insert_value(params, key, created);
}

static bool map_insert_string(struct mgp_map *map, const char *key, const char *value,
                              struct mgp_memory *memory) {
  return params_insert_string(map, key, value, memory);
}

static bool map_insert_int(struct mgp_map *map, const char *key, int64_t value,
                           struct mgp_memory *memory) {
  return params_insert_int(map, key, value, memory);
}

static bool map_insert_bool(struct mgp_map *map, const char *key, bool value,
                            struct mgp_memory *memory) {
  return params_insert_bool(map, key, value, memory);
}

static struct mgp_map *make_model_params(const char *model, struct mgp_memory *memory) {
  struct mgp_map *params = NULL;
  if (!mg_ok(mgp_unordered_map_make_empty(memory, &params)) || params == NULL ||
      !params_insert_string(params, "model", model, memory)) {
    if (params != NULL) {
      mgp_map_destroy(params);
    }
    return NULL;
  }
  return params;
}

static bool execute_drain(struct mgp_graph *graph, struct mgp_memory *memory, const char *query,
                          struct mgp_map *params) {
  struct mgp_execution_result *execution = NULL;
  if (!mg_ok(mgp_execute_query(graph, memory, query, params, &execution)) || execution == NULL) {
    return false;
  }
  bool ok = true;
  for (;;) {
    struct mgp_map *row = NULL;
    if (!mg_ok(mgp_pull_one(execution, graph, memory, &row))) {
      ok = false;
      break;
    }
    if (row == NULL) {
      break;
    }
  }
  mgp_execution_result_destroy(execution);
  return ok;
}

static bool execute_model_query(struct mgp_graph *graph, struct mgp_memory *memory,
                                const char *model, const char *query) {
  struct mgp_map *params = make_model_params(model, memory);
  if (params == NULL) {
    return false;
  }
  const bool ok = execute_drain(graph, memory, query, params);
  mgp_map_destroy(params);
  return ok;
}

static bool row_string(struct mgp_map *row, const char *field, const char **value) {
  struct mgp_value *entry = NULL;
  return mg_ok(mgp_map_at(row, field, &entry)) && entry != NULL &&
         mg_ok(mgp_value_get_string(entry, value)) && *value != NULL;
}

static bool row_int(struct mgp_map *row, const char *field, int64_t *value) {
  struct mgp_value *entry = NULL;
  return mg_ok(mgp_map_at(row, field, &entry)) && entry != NULL &&
         mg_ok(mgp_value_get_int(entry, value));
}

static bool row_bool(struct mgp_map *row, const char *field, bool *value) {
  struct mgp_value *entry = NULL;
  int raw = 0;
  if (!mg_ok(mgp_map_at(row, field, &entry)) || entry == NULL ||
      !mg_ok(mgp_value_get_bool(entry, &raw))) {
    return false;
  }
  *value = raw != 0;
  return true;
}

static bool uint64_to_int64(uint64_t value, int64_t *converted) {
  if (value > (uint64_t)INT64_MAX) {
    return false;
  }
  *converted = (int64_t)value;
  return true;
}

static bool size_to_int64(size_t value, int64_t *converted) {
  if (value > (size_t)INT64_MAX) {
    return false;
  }
  *converted = (int64_t)value;
  return true;
}

static int64_t index_to_int64(size_t value) {
  int64_t converted = -1;
  return value == SG_INDEX_NONE || !size_to_int64(value, &converted) ? -1 : converted;
}

static bool int64_to_index(int64_t value, size_t *converted) {
  if (value < -1 || (uint64_t)value > (uint64_t)SIZE_MAX) {
    return false;
  }
  *converted = value == -1 ? SG_INDEX_NONE : (size_t)value;
  return true;
}

static sg_status load_metadata(struct mgp_graph *graph, struct mgp_memory *memory,
                               const char *model, model_metadata *metadata) {
  static const char *query = "MATCH (m:SyncModel {model: $model}) "
                             "RETURN coalesce(m.generation, 0) AS generation, "
                             "coalesce(m.dirty, true) AS dirty, "
                             "coalesce(m.prepared_generation, -1) AS prepared_generation";
  struct mgp_map *params = make_model_params(model, memory);
  if (params == NULL) {
    return SG_ERR_ALLOC;
  }
  struct mgp_execution_result *execution = NULL;
  sg_status status = SG_OK;
  if (!mg_ok(mgp_execute_query(graph, memory, query, params, &execution)) || execution == NULL) {
    status = SG_ERR_INVALID_MODEL;
  }
  struct mgp_map *row = NULL;
  int64_t generation = -1;
  int64_t prepared_generation = -1;
  bool dirty = true;
  if (status == SG_OK &&
      (!mg_ok(mgp_pull_one(execution, graph, memory, &row)) || row == NULL ||
       !row_int(row, "generation", &generation) || !row_bool(row, "dirty", &dirty) ||
       !row_int(row, "prepared_generation", &prepared_generation) || generation < 0)) {
    status = SG_ERR_NOT_FOUND;
  }
  struct mgp_map *extra = NULL;
  if (status == SG_OK &&
      (!mg_ok(mgp_pull_one(execution, graph, memory, &extra)) || extra != NULL)) {
    status = SG_ERR_INVALID_MODEL;
  }
  if (status == SG_OK) {
    metadata->generation = (uint64_t)generation;
    metadata->dirty = dirty;
    metadata->prepared = !dirty && prepared_generation == generation;
  }
  if (execution != NULL) {
    mgp_execution_result_destroy(execution);
  }
  mgp_map_destroy(params);
  return status;
}

static sg_status add_domain_value(sg_automaton_builder *builder, domain_kind kind,
                                  const char *value) {
  switch (kind) {
  case DOMAIN_STATE:
    return sg_automaton_builder_add_state(builder, value);
  case DOMAIN_ACTION:
    return sg_automaton_builder_add_action(builder, value);
  case DOMAIN_OUTPUT:
    return sg_automaton_builder_add_output(builder, value);
  }
  return SG_ERR_INVALID_ARGUMENT;
}

static sg_status load_domain(struct mgp_graph *graph, struct mgp_memory *memory, const char *model,
                             const char *query, const char *field, domain_kind kind,
                             sg_automaton_builder *builder) {
  struct mgp_map *params = make_model_params(model, memory);
  if (params == NULL) {
    return SG_ERR_ALLOC;
  }
  struct mgp_execution_result *execution = NULL;
  sg_status status = SG_OK;
  if (!mg_ok(mgp_execute_query(graph, memory, query, params, &execution)) || execution == NULL) {
    status = SG_ERR_INVALID_MODEL;
  }
  while (status == SG_OK) {
    struct mgp_map *row = NULL;
    if (!mg_ok(mgp_pull_one(execution, graph, memory, &row))) {
      status = SG_ERR_INVALID_MODEL;
      break;
    }
    if (row == NULL) {
      break;
    }
    const char *value = NULL;
    if (!row_string(row, field, &value)) {
      status = SG_ERR_INVALID_MODEL;
      break;
    }
    status = add_domain_value(builder, kind, value);
  }
  if (execution != NULL) {
    mgp_execution_result_destroy(execution);
  }
  mgp_map_destroy(params);
  return status;
}

static sg_status load_transitions(struct mgp_graph *graph, struct mgp_memory *memory,
                                  const char *model, sg_automaton_builder *builder) {
  static const char *query =
      "MATCH (src:SyncState {model: $model})-"
      "[t:SYNC_TRANS {model: $model}]->(dst:SyncState {model: $model}) "
      "RETURN src.state_key AS source, t.action_key AS action, dst.state_key AS target "
      "ORDER BY source, action, target";
  struct mgp_map *params = make_model_params(model, memory);
  if (params == NULL) {
    return SG_ERR_ALLOC;
  }
  struct mgp_execution_result *execution = NULL;
  sg_status status = SG_OK;
  if (!mg_ok(mgp_execute_query(graph, memory, query, params, &execution)) || execution == NULL) {
    status = SG_ERR_INVALID_MODEL;
  }
  while (status == SG_OK) {
    struct mgp_map *row = NULL;
    if (!mg_ok(mgp_pull_one(execution, graph, memory, &row))) {
      status = SG_ERR_INVALID_MODEL;
      break;
    }
    if (row == NULL) {
      break;
    }
    const char *source = NULL;
    const char *action = NULL;
    const char *target = NULL;
    if (!row_string(row, "source", &source) || !row_string(row, "action", &action) ||
        !row_string(row, "target", &target)) {
      status = SG_ERR_INVALID_MODEL;
      break;
    }
    status = sg_automaton_builder_add_transition(builder, source, action, target);
  }
  if (execution != NULL) {
    mgp_execution_result_destroy(execution);
  }
  mgp_map_destroy(params);
  return status;
}

static sg_status load_observations(struct mgp_graph *graph, struct mgp_memory *memory,
                                   const char *model, sg_automaton_builder *builder) {
  static const char *query =
      "MATCH (src:SyncState {model: $model})-"
      "[o:SYNC_OBS {model: $model}]->(output:SyncOutput {model: $model}) "
      "RETURN src.state_key AS source, o.action_key AS action, output.output_key AS output "
      "ORDER BY source, action, output";
  struct mgp_map *params = make_model_params(model, memory);
  if (params == NULL) {
    return SG_ERR_ALLOC;
  }
  struct mgp_execution_result *execution = NULL;
  sg_status status = SG_OK;
  if (!mg_ok(mgp_execute_query(graph, memory, query, params, &execution)) || execution == NULL) {
    status = SG_ERR_INVALID_MODEL;
  }
  while (status == SG_OK) {
    struct mgp_map *row = NULL;
    if (!mg_ok(mgp_pull_one(execution, graph, memory, &row))) {
      status = SG_ERR_INVALID_MODEL;
      break;
    }
    if (row == NULL) {
      break;
    }
    const char *source = NULL;
    const char *action = NULL;
    const char *output = NULL;
    if (!row_string(row, "source", &source) || !row_string(row, "action", &action) ||
        !row_string(row, "output", &output)) {
      status = SG_ERR_INVALID_MODEL;
      break;
    }
    status = sg_automaton_builder_add_observation(builder, source, action, output);
  }
  if (execution != NULL) {
    mgp_execution_result_destroy(execution);
  }
  mgp_map_destroy(params);
  return status;
}

static sg_status load_automaton(struct mgp_graph *graph, struct mgp_memory *memory,
                                const char *model, model_metadata *metadata,
                                sg_automaton **automaton) {
  static const char *state_query =
      "MATCH (s:SyncState {model: $model}) RETURN s.state_key AS state_key "
      "ORDER BY s.state_id, s.state_key";
  static const char *action_query =
      "MATCH (a:SyncAction {model: $model}) RETURN a.action_key AS action_key "
      "ORDER BY a.action_id, a.action_key";
  static const char *output_query =
      "MATCH (o:SyncOutput {model: $model}) RETURN o.output_key AS output_key "
      "ORDER BY o.output_id, o.output_key";

  sg_status status = load_metadata(graph, memory, model, metadata);
  sg_automaton_builder *builder = NULL;
  if (status == SG_OK) {
    status = sg_automaton_builder_init(&builder);
  }
  if (status == SG_OK) {
    status = load_domain(graph, memory, model, state_query, "state_key", DOMAIN_STATE, builder);
  }
  if (status == SG_OK) {
    status = load_domain(graph, memory, model, action_query, "action_key", DOMAIN_ACTION, builder);
  }
  if (status == SG_OK) {
    status = load_domain(graph, memory, model, output_query, "output_key", DOMAIN_OUTPUT, builder);
  }
  if (status == SG_OK) {
    status = load_transitions(graph, memory, model, builder);
  }
  if (status == SG_OK) {
    status = load_observations(graph, memory, model, builder);
  }
  if (status == SG_OK) {
    status = sg_automaton_builder_build(builder, metadata->generation, automaton);
  }
  sg_automaton_builder_free(builder);
  return status;
}

static bool pair_count_for_states(size_t state_count, size_t *pair_count) {
  if (state_count == SIZE_MAX) {
    return false;
  }
  const size_t second = state_count + 1U;
  if (state_count != 0U && second > SIZE_MAX / state_count) {
    return false;
  }
  *pair_count = (state_count * second) / 2U;
  return true;
}

static sg_status load_oracle(struct mgp_graph *graph, struct mgp_memory *memory, const char *model,
                             const sg_automaton *automaton, sg_pair_oracle **oracle) {
  static const char *query =
      "MATCH (p:SyncPair {model: $model, generation: $generation}) "
      "RETURN p.pair_id AS pair_id, p.mergeable AS mergeable, "
      "p.merge_distance AS merge_distance, p.merge_action_id AS merge_action_id, "
      "p.merge_next_pair AS merge_next_pair, p.resolvable AS resolvable, "
      "p.resolution_distance AS resolution_distance, "
      "p.resolution_action_id AS resolution_action_id, "
      "p.resolution_next_pair AS resolution_next_pair ORDER BY pair_id";
  size_t expected_count = 0U;
  if (!pair_count_for_states(sg_automaton_state_count(automaton), &expected_count)) {
    return SG_ERR_ALLOC;
  }
  if (expected_count == 0U) {
    return SG_ERR_INVALID_MODEL;
  }
  sg_pair_record *records = calloc(expected_count, sizeof(*records));
  if (records == NULL) {
    return SG_ERR_ALLOC;
  }
  struct mgp_map *params = make_model_params(model, memory);
  int64_t generation = 0;
  if (params == NULL || !uint64_to_int64(sg_automaton_generation(automaton), &generation) ||
      !params_insert_int(params, "generation", generation, memory)) {
    if (params != NULL) {
      mgp_map_destroy(params);
    }
    free(records);
    return SG_ERR_ALLOC;
  }
  struct mgp_execution_result *execution = NULL;
  sg_status status = SG_OK;
  if (!mg_ok(mgp_execute_query(graph, memory, query, params, &execution)) || execution == NULL) {
    status = SG_ERR_INVALID_MODEL;
  }
  size_t count = 0U;
  while (status == SG_OK) {
    struct mgp_map *row = NULL;
    if (!mg_ok(mgp_pull_one(execution, graph, memory, &row))) {
      status = SG_ERR_INVALID_MODEL;
      break;
    }
    if (row == NULL) {
      break;
    }
    if (count >= expected_count) {
      status = SG_ERR_INVALID_MODEL;
      break;
    }
    int64_t pair = -1;
    int64_t merge_distance = -1;
    int64_t merge_action = -1;
    int64_t merge_next = -1;
    int64_t resolution_distance = -1;
    int64_t resolution_action = -1;
    int64_t resolution_next = -1;
    bool mergeable = false;
    bool resolvable = false;
    if (!row_int(row, "pair_id", &pair) || !row_bool(row, "mergeable", &mergeable) ||
        !row_int(row, "merge_distance", &merge_distance) ||
        !row_int(row, "merge_action_id", &merge_action) ||
        !row_int(row, "merge_next_pair", &merge_next) ||
        !row_bool(row, "resolvable", &resolvable) ||
        !row_int(row, "resolution_distance", &resolution_distance) ||
        !row_int(row, "resolution_action_id", &resolution_action) ||
        !row_int(row, "resolution_next_pair", &resolution_next) || pair < 0 ||
        !int64_to_index(merge_distance, &records[count].merge_distance) ||
        !int64_to_index(merge_action, &records[count].merge_action) ||
        !int64_to_index(merge_next, &records[count].merge_next_pair) ||
        !int64_to_index(resolution_distance, &records[count].resolution_distance) ||
        !int64_to_index(resolution_action, &records[count].resolution_action) ||
        !int64_to_index(resolution_next, &records[count].resolution_next_pair)) {
      status = SG_ERR_INVALID_MODEL;
      break;
    }
    records[count].pair = (size_t)pair;
    records[count].mergeable = mergeable;
    records[count].resolvable = resolvable;
    ++count;
  }
  if (execution != NULL) {
    mgp_execution_result_destroy(execution);
  }
  mgp_map_destroy(params);
  if (status == SG_OK) {
    status = sg_pair_oracle_restore(automaton, records, count, oracle);
  }
  free(records);
  return status;
}

static bool list_to_ids(const sg_automaton *automaton, struct mgp_value *value, bool actions,
                        bool allow_empty, size_t **ids, size_t *count) {
  *ids = NULL;
  *count = 0U;
  struct mgp_list *list = NULL;
  if (!mg_ok(mgp_value_get_list(value, &list)) || list == NULL ||
      !mg_ok(mgp_list_size(list, count)) || (!allow_empty && *count == 0U)) {
    return false;
  }
  size_t *created = calloc(*count == 0U ? 1U : *count, sizeof(*created));
  if (created == NULL) {
    return false;
  }
  for (size_t index = 0U; index < *count; ++index) {
    struct mgp_value *item = NULL;
    const char *key = NULL;
    if (!mg_ok(mgp_list_at(list, index, &item)) || item == NULL ||
        !mg_ok(mgp_value_get_string(item, &key)) || key == NULL) {
      free(created);
      return false;
    }
    const sg_status status = actions ? sg_automaton_find_action(automaton, key, &created[index])
                                     : sg_automaton_find_state(automaton, key, &created[index]);
    if (status != SG_OK) {
      free(created);
      return false;
    }
  }
  *ids = created;
  return true;
}

static bool argument_to_ids(const sg_automaton *automaton, struct mgp_list *arguments, size_t index,
                            bool actions, bool allow_empty, size_t **ids, size_t *count) {
  struct mgp_value *value = NULL;
  return get_arg(arguments, index, &value) &&
         list_to_ids(automaton, value, actions, allow_empty, ids, count);
}

static bool argument_to_word(const sg_automaton *automaton, struct mgp_list *arguments,
                             size_t index, sg_word *word) {
  size_t *actions = NULL;
  size_t count = 0U;
  if (!argument_to_ids(automaton, arguments, index, true, true, &actions, &count) ||
      sg_word_init(word) != SG_OK) {
    free(actions);
    return false;
  }
  bool ok = true;
  for (size_t position = 0U; position < count; ++position) {
    if (sg_word_append(word, actions[position]) != SG_OK) {
      ok = false;
      break;
    }
  }
  free(actions);
  if (!ok) {
    sg_word_free(word);
  }
  return ok;
}

static struct mgp_value *make_key_list(const sg_automaton *automaton, const size_t *ids,
                                       size_t count, bool outputs, struct mgp_memory *memory) {
  struct mgp_list *list = NULL;
  if (!mg_ok(mgp_list_make_empty(count, memory, &list)) || list == NULL) {
    return NULL;
  }
  for (size_t index = 0U; index < count; ++index) {
    const char *key = outputs ? sg_automaton_output_key(automaton, ids[index])
                              : sg_automaton_state_key(automaton, ids[index]);
    struct mgp_value *item = NULL;
    if (key == NULL || !mg_ok(mgp_value_make_string(key, memory, &item)) || item == NULL ||
        !mg_ok(mgp_list_append_move(list, item))) {
      if (item != NULL) {
        mgp_value_destroy(item);
      }
      mgp_list_destroy(list);
      return NULL;
    }
  }
  struct mgp_value *value = NULL;
  if (!mg_ok(mgp_value_make_list(list, &value)) || value == NULL) {
    mgp_list_destroy(list);
    return NULL;
  }
  return value;
}

static bool insert_key_list(struct mgp_result_record *record, const char *field,
                            const sg_automaton *automaton, const size_t *ids, size_t count,
                            bool outputs, struct mgp_memory *memory) {
  return insert_value(record, field, make_key_list(automaton, ids, count, outputs, memory));
}

static bool insert_empty_list(struct mgp_result_record *record, const char *field,
                              struct mgp_memory *memory) {
  struct mgp_list *list = NULL;
  if (!mg_ok(mgp_list_make_empty(0U, memory, &list)) || list == NULL) {
    return false;
  }
  struct mgp_value *value = NULL;
  if (!mg_ok(mgp_value_make_list(list, &value)) || value == NULL) {
    mgp_list_destroy(list);
    return false;
  }
  return insert_value(record, field, value);
}

static bool insert_word(struct mgp_result_record *record, const sg_automaton *automaton,
                        const sg_word *word, struct mgp_memory *memory) {
  struct mgp_list *list = NULL;
  if (!mg_ok(mgp_list_make_empty(word->length, memory, &list)) || list == NULL) {
    return false;
  }
  for (size_t index = 0U; index < word->length; ++index) {
    const char *key = sg_automaton_action_key(automaton, word->actions[index]);
    struct mgp_value *item = NULL;
    if (key == NULL || !mg_ok(mgp_value_make_string(key, memory, &item)) || item == NULL ||
        !mg_ok(mgp_list_append_move(list, item))) {
      if (item != NULL) {
        mgp_value_destroy(item);
      }
      mgp_list_destroy(list);
      return false;
    }
  }
  struct mgp_value *value = NULL;
  if (!mg_ok(mgp_value_make_list(list, &value)) || value == NULL) {
    mgp_list_destroy(list);
    return false;
  }
  return insert_value(record, "word", value);
}

static const char *outcome_reason(sg_plan_outcome outcome) {
  switch (outcome) {
  case SG_OUTCOME_PLAN:
    return "plan found";
  case SG_OUTCOME_ALREADY_SATISFIED:
    return "objective already satisfied";
  case SG_OUTCOME_NO_PLAN:
    return "no qualifying word exists";
  case SG_OUTCOME_RESOURCE_BOUND:
    return "search budget exhausted";
  }
  return "unknown outcome";
}

static bool append_pair_record(struct mgp_list *list, const sg_automaton *automaton,
                               const sg_pair_oracle *oracle, size_t pair,
                               struct mgp_memory *memory) {
  sg_pair_record record = {0};
  size_t first = 0U;
  size_t second = 0U;
  if (sg_pair_oracle_record(oracle, pair, &record) != SG_OK ||
      sg_pair_oracle_pair_states(oracle, pair, &first, &second) != SG_OK) {
    return false;
  }
  const char *merge_action = record.merge_action == SG_INDEX_NONE
                                 ? ""
                                 : sg_automaton_action_key(automaton, record.merge_action);
  const char *resolution_action =
      record.resolution_action == SG_INDEX_NONE
          ? ""
          : sg_automaton_action_key(automaton, record.resolution_action);
  int64_t pair_id = 0;
  int64_t first_id = 0;
  int64_t second_id = 0;
  if (merge_action == NULL || resolution_action == NULL || !size_to_int64(pair, &pair_id) ||
      !size_to_int64(first, &first_id) || !size_to_int64(second, &second_id)) {
    return false;
  }
  struct mgp_map *map = NULL;
  if (!mg_ok(mgp_unordered_map_make_empty(memory, &map)) || map == NULL) {
    return false;
  }
  const bool populated =
      map_insert_int(map, "pair_id", pair_id, memory) &&
      map_insert_string(map, "first_key", sg_automaton_state_key(automaton, first), memory) &&
      map_insert_int(map, "first_id", first_id, memory) &&
      map_insert_string(map, "second_key", sg_automaton_state_key(automaton, second), memory) &&
      map_insert_int(map, "second_id", second_id, memory) &&
      map_insert_bool(map, "mergeable", record.mergeable, memory) &&
      map_insert_int(map, "merge_distance", index_to_int64(record.merge_distance), memory) &&
      map_insert_string(map, "merge_action", merge_action, memory) &&
      map_insert_int(map, "merge_action_id", index_to_int64(record.merge_action), memory) &&
      map_insert_int(map, "merge_next_pair", index_to_int64(record.merge_next_pair), memory) &&
      map_insert_bool(map, "resolvable", record.resolvable, memory) &&
      map_insert_int(map, "resolution_distance", index_to_int64(record.resolution_distance),
                     memory) &&
      map_insert_string(map, "resolution_action", resolution_action, memory) &&
      map_insert_int(map, "resolution_action_id", index_to_int64(record.resolution_action),
                     memory) &&
      map_insert_int(map, "resolution_next_pair", index_to_int64(record.resolution_next_pair),
                     memory);
  if (!populated) {
    mgp_map_destroy(map);
    return false;
  }
  struct mgp_value *value = NULL;
  if (!mg_ok(mgp_value_make_map(map, &value)) || value == NULL) {
    mgp_map_destroy(map);
    return false;
  }
  if (!mg_ok(mgp_list_append_move(list, value))) {
    mgp_value_destroy(value);
    return false;
  }
  return true;
}

static bool execute_pair_batch(struct mgp_graph *graph, struct mgp_memory *memory,
                               const char *model, const sg_automaton *automaton,
                               const sg_pair_oracle *oracle, size_t first_pair, size_t pair_count) {
  static const char *query =
      "UNWIND $records AS r "
      "CREATE (:SyncPair {model: $model, generation: $generation, pair_id: r.pair_id, "
      "first_key: r.first_key, first_id: r.first_id, second_key: r.second_key, "
      "second_id: r.second_id, mergeable: r.mergeable, merge_distance: r.merge_distance, "
      "merge_action: r.merge_action, merge_action_id: r.merge_action_id, "
      "merge_next_pair: r.merge_next_pair, resolvable: r.resolvable, "
      "resolution_distance: r.resolution_distance, resolution_action: r.resolution_action, "
      "resolution_action_id: r.resolution_action_id, "
      "resolution_next_pair: r.resolution_next_pair})";
  struct mgp_list *records = NULL;
  if (!mg_ok(mgp_list_make_empty(pair_count, memory, &records)) || records == NULL) {
    return false;
  }
  bool ok = true;
  for (size_t offset = 0U; ok && offset < pair_count; ++offset) {
    ok = append_pair_record(records, automaton, oracle, first_pair + offset, memory);
  }
  struct mgp_value *records_value = NULL;
  if (ok) {
    ok = mg_ok(mgp_value_make_list(records, &records_value)) && records_value != NULL;
  }
  if (!ok) {
    mgp_list_destroy(records);
    return false;
  }
  struct mgp_map *params = make_model_params(model, memory);
  int64_t generation = 0;
  if (params == NULL || !uint64_to_int64(sg_automaton_generation(automaton), &generation) ||
      !params_insert_int(params, "generation", generation, memory) ||
      !params_insert_value(params, "records", records_value)) {
    if (params != NULL) {
      mgp_map_destroy(params);
    }
    return false;
  }
  ok = execute_drain(graph, memory, query, params);
  mgp_map_destroy(params);
  return ok;
}

static bool append_pair_edge(struct mgp_list *list, const sg_automaton *automaton,
                             const sg_pair_oracle *oracle, size_t edge, struct mgp_memory *memory) {
  const size_t action_count = sg_automaton_action_count(automaton);
  const size_t pair = edge / action_count;
  const size_t action = edge % action_count;
  size_t next_pair = 0U;
  bool outputs_differ = false;
  if (sg_pair_oracle_pair_step(oracle, pair, action, &next_pair, &outputs_differ) != SG_OK) {
    return false;
  }
  int64_t pair_id = 0;
  int64_t action_id = 0;
  int64_t next_pair_id = 0;
  if (!size_to_int64(pair, &pair_id) || !size_to_int64(action, &action_id) ||
      !size_to_int64(next_pair, &next_pair_id)) {
    return false;
  }
  struct mgp_map *map = NULL;
  if (!mg_ok(mgp_unordered_map_make_empty(memory, &map)) || map == NULL) {
    return false;
  }
  const bool populated =
      map_insert_int(map, "pair_id", pair_id, memory) &&
      map_insert_string(map, "action", sg_automaton_action_key(automaton, action), memory) &&
      map_insert_int(map, "action_id", action_id, memory) &&
      map_insert_int(map, "next_pair", next_pair_id, memory) &&
      map_insert_bool(map, "outputs_differ", outputs_differ, memory);
  if (!populated) {
    mgp_map_destroy(map);
    return false;
  }
  struct mgp_value *value = NULL;
  if (!mg_ok(mgp_value_make_map(map, &value)) || value == NULL) {
    mgp_map_destroy(map);
    return false;
  }
  if (!mg_ok(mgp_list_append_move(list, value))) {
    mgp_value_destroy(value);
    return false;
  }
  return true;
}

static bool execute_edge_batch(struct mgp_graph *graph, struct mgp_memory *memory,
                               const char *model, const sg_automaton *automaton,
                               const sg_pair_oracle *oracle, size_t first_edge, size_t edge_count) {
  static const char *query =
      "UNWIND $edges AS e "
      "MATCH (p:SyncPair {model: $model, generation: $generation, pair_id: e.pair_id}), "
      "(n:SyncPair {model: $model, generation: $generation, pair_id: e.next_pair}) "
      "CREATE (p)-[:PAIR_NEXT {model: $model, generation: $generation, action: e.action, "
      "action_id: e.action_id, outputs_differ: e.outputs_differ}]->(n) "
      "CREATE (n)-[:PAIR_PRE {model: $model, generation: $generation, action: e.action, "
      "action_id: e.action_id, outputs_differ: e.outputs_differ}]->(p)";
  struct mgp_list *edges = NULL;
  if (!mg_ok(mgp_list_make_empty(edge_count, memory, &edges)) || edges == NULL) {
    return false;
  }
  bool ok = true;
  for (size_t offset = 0U; ok && offset < edge_count; ++offset) {
    ok = append_pair_edge(edges, automaton, oracle, first_edge + offset, memory);
  }
  struct mgp_value *edges_value = NULL;
  if (ok) {
    ok = mg_ok(mgp_value_make_list(edges, &edges_value)) && edges_value != NULL;
  }
  if (!ok) {
    mgp_list_destroy(edges);
    return false;
  }
  struct mgp_map *params = make_model_params(model, memory);
  int64_t generation = 0;
  if (params == NULL || !uint64_to_int64(sg_automaton_generation(automaton), &generation) ||
      !params_insert_int(params, "generation", generation, memory) ||
      !params_insert_value(params, "edges", edges_value)) {
    if (params != NULL) {
      mgp_map_destroy(params);
    }
    return false;
  }
  ok = execute_drain(graph, memory, query, params);
  mgp_map_destroy(params);
  return ok;
}

static bool materialize_oracle(struct mgp_graph *graph, struct mgp_memory *memory,
                               const char *model, const sg_automaton *automaton,
                               const sg_pair_oracle *oracle, bool materialize_edges) {
  static const char *clear_query = "MATCH (p:SyncPair {model: $model}) DETACH DELETE p";
  static const char *finish_query = "MATCH (m:SyncModel {model: $model}) "
                                    "SET m.dirty = false, m.prepared_generation = $generation, "
                                    "m.pair_edges_materialized = $materialize_edges";
  if (!execute_model_query(graph, memory, model, clear_query)) {
    return false;
  }
  const size_t pair_count = sg_pair_oracle_pair_count(oracle);
  for (size_t first = 0U; first < pair_count; first += SG_MATERIALIZE_BATCH) {
    const size_t remaining = pair_count - first;
    const size_t count = remaining < SG_MATERIALIZE_BATCH ? remaining : SG_MATERIALIZE_BATCH;
    if (!execute_pair_batch(graph, memory, model, automaton, oracle, first, count)) {
      return false;
    }
  }
  if (materialize_edges) {
    const size_t edge_count = sg_pair_oracle_pair_edge_count(oracle);
    for (size_t first = 0U; first < edge_count; first += SG_MATERIALIZE_BATCH) {
      const size_t remaining = edge_count - first;
      const size_t count = remaining < SG_MATERIALIZE_BATCH ? remaining : SG_MATERIALIZE_BATCH;
      if (!execute_edge_batch(graph, memory, model, automaton, oracle, first, count)) {
        return false;
      }
    }
  }
  struct mgp_map *params = make_model_params(model, memory);
  int64_t generation = 0;
  if (params == NULL || !uint64_to_int64(sg_automaton_generation(automaton), &generation) ||
      !params_insert_int(params, "generation", generation, memory) ||
      !params_insert_bool(params, "materialize_edges", materialize_edges, memory)) {
    if (params != NULL) {
      mgp_map_destroy(params);
    }
    return false;
  }
  const bool ok = execute_drain(graph, memory, finish_query, params);
  mgp_map_destroy(params);
  return ok;
}

/* The automaton half of runtime_load, on its own.
 *
 * Three procedures - plan_goal, explain_plan and validate_update - call
 * sg_* entry points that take only the automaton. They were nonetheless
 * loading the pair oracle through runtime_load and freeing it unused, and
 * load_oracle reads one SyncPair node per state pair: 353,220 rows for an
 * 840-state model. Measured against that model, a plan_goal call with
 * nothing to search cost 956 ms, of which the search was ~10 ms; the rest
 * was this load. validate_update runs after every executed action, so a
 * robot paid it once per letter. */
static bool runtime_load_automaton(struct mgp_graph *graph, struct mgp_memory *memory,
                                   const char *model, sg_automaton **automaton,
                                   struct mgp_result *result) {
  model_metadata metadata = {0};
  sg_status status = load_automaton(graph, memory, model, &metadata, automaton);
  if (status != SG_OK) {
    set_status_error(result, "model extraction failed", status);
    return false;
  }
  if (!metadata.prepared) {
    set_error(result, "model is dirty or has not been prepared");
    sg_automaton_free(*automaton);
    *automaton = NULL;
    return false;
  }
  return true;
}

static bool runtime_load(struct mgp_graph *graph, struct mgp_memory *memory, const char *model,
                         sg_automaton **automaton, sg_pair_oracle **oracle,
                         struct mgp_result *result) {
  if (!runtime_load_automaton(graph, memory, model, automaton, result)) {
    return false;
  }
  sg_status status = load_oracle(graph, memory, model, *automaton, oracle);
  if (status != SG_OK) {
    set_status_error(result, "prepared oracle is invalid", status);
    sg_automaton_free(*automaton);
    *automaton = NULL;
    return false;
  }
  return true;
}

static void prepare_model_cb(struct mgp_list *arguments, struct mgp_graph *graph,
                             struct mgp_result *result, struct mgp_memory *memory) {
  const char *model = NULL;
  bool materialize_edges = false;
  if (!get_string_arg(arguments, 0U, &model) ||
      !get_bool_arg_default(arguments, 1U, false, &materialize_edges)) {
    set_error(result, "expected model and optional materialize_pair_edges boolean");
    return;
  }
  model_metadata metadata = {0};
  sg_automaton *automaton = NULL;
  sg_status status = load_automaton(graph, memory, model, &metadata, &automaton);
  if (status != SG_OK) {
    set_status_error(result, "model extraction failed", status);
    return;
  }
  sg_pair_oracle *oracle = NULL;
  status = sg_pair_oracle_build(automaton, &oracle);
  const size_t pair_count = status == SG_OK ? sg_pair_oracle_pair_count(oracle) : 0U;
  const size_t edge_count = status == SG_OK ? sg_pair_oracle_pair_edge_count(oracle) : 0U;
  int64_t ignored = 0;
  if (status == SG_OK && (!size_to_int64(pair_count, &ignored) ||
                          !size_to_int64(sg_automaton_action_count(automaton), &ignored))) {
    status = SG_ERR_RESOURCE_BOUND;
  }
  if (status == SG_OK &&
      !materialize_oracle(graph, memory, model, automaton, oracle, materialize_edges)) {
    status = SG_ERR_INVALID_MODEL;
  }
  if (status != SG_OK) {
    set_status_error(result, "model preparation failed", status);
    sg_pair_oracle_free(oracle);
    sg_automaton_free(automaton);
    return;
  }
  struct mgp_result_record *record = NULL;
  int64_t generation = 0;
  int64_t states = 0;
  int64_t actions = 0;
  int64_t outputs = 0;
  int64_t transitions = 0;
  int64_t pairs = 0;
  int64_t edges = 0;
  const bool converted = uint64_to_int64(sg_automaton_generation(automaton), &generation) &&
                         size_to_int64(sg_automaton_state_count(automaton), &states) &&
                         size_to_int64(sg_automaton_action_count(automaton), &actions) &&
                         size_to_int64(sg_automaton_output_count(automaton), &outputs) &&
                         size_to_int64(sg_automaton_transition_count(automaton), &transitions) &&
                         size_to_int64(pair_count, &pairs) && size_to_int64(edge_count, &edges);
  if (!converted || !new_record(result, &record) ||
      !insert_string(record, "status", "OK", memory) ||
      !insert_int(record, "generation", generation, memory) ||
      !insert_int(record, "states", states, memory) ||
      !insert_int(record, "actions", actions, memory) ||
      !insert_int(record, "outputs", outputs, memory) ||
      !insert_int(record, "transitions", transitions, memory) ||
      !insert_int(record, "pairs", pairs, memory) ||
      !insert_int(record, "pair_edges", edges, memory) ||
      !insert_int(record, "mergeable_pairs", (int64_t)sg_pair_oracle_mergeable_pair_count(oracle),
                  memory) ||
      !insert_int(record, "resolvable_pairs", (int64_t)sg_pair_oracle_resolvable_pair_count(oracle),
                  memory) ||
      !insert_bool(record, "materialized_pair_edges", materialize_edges, memory)) {
    set_error(result, "failed to create preparation result");
  }
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);
}

static bool insert_plan_common(struct mgp_result_record *record, const sg_automaton *automaton,
                               const sg_plan_result *plan, struct mgp_memory *memory) {
  int64_t generation = 0;
  int64_t length = 0;
  int64_t expansions = 0;
  int64_t planning_time = 0;
  return uint64_to_int64(plan->generation, &generation) &&
         size_to_int64(plan->word.length, &length) &&
         size_to_int64(plan->expansions, &expansions) &&
         uint64_to_int64(plan->planning_time_us, &planning_time) &&
         insert_string(record, "status", "OK", memory) &&
         insert_string(record, "outcome", sg_plan_outcome_name(plan->outcome), memory) &&
         insert_string(record, "reason", outcome_reason(plan->outcome), memory) &&
         insert_string(record, "method", sg_plan_method_name(plan->method), memory) &&
         insert_word(record, automaton, &plan->word, memory) &&
         insert_int(record, "length", length, memory) &&
         insert_int(record, "expansions", expansions, memory) &&
         insert_int(record, "generation", generation, memory) &&
         insert_int(record, "planning_time_us", planning_time, memory);
}

static void plan_sync_cb(struct mgp_list *arguments, struct mgp_graph *graph,
                         struct mgp_result *result, struct mgp_memory *memory) {
  const char *model = NULL;
  int64_t budget = 0;
  if (!get_string_arg(arguments, 0U, &model) || !get_int_arg(arguments, 2U, &budget) ||
      budget <= 0 || (uint64_t)budget > (uint64_t)SIZE_MAX) {
    set_error(result, "expected model, nonempty hypotheses, and positive budget");
    return;
  }
  sg_automaton *automaton = NULL;
  sg_pair_oracle *oracle = NULL;
  if (!runtime_load(graph, memory, model, &automaton, &oracle, result)) {
    return;
  }
  size_t *hypotheses = NULL;
  size_t hypothesis_count = 0U;
  if (!argument_to_ids(automaton, arguments, 1U, false, false, &hypotheses, &hypothesis_count)) {
    set_error(result, "hypotheses must be a nonempty list of prepared state keys");
    sg_pair_oracle_free(oracle);
    sg_automaton_free(automaton);
    return;
  }
  sg_plan_result plan = {0};
  const sg_status status =
      sg_plan_sync(automaton, oracle, hypotheses, hypothesis_count, (size_t)budget, &plan);
  free(hypotheses);
  if (status != SG_OK) {
    set_status_error(result, "synchronization planning failed", status);
  } else {
    struct mgp_result_record *record = NULL;
    int64_t final_support = 0;
    const char *final_state = plan.final_state == SG_INDEX_NONE
                                  ? ""
                                  : sg_automaton_state_key(automaton, plan.final_state);
    if (final_state == NULL || !size_to_int64(plan.final_support_size, &final_support) ||
        !new_record(result, &record) || !insert_plan_common(record, automaton, &plan, memory) ||
        !insert_string(record, "final_state_key", final_state, memory) ||
        !insert_int(record, "final_support_size", final_support, memory)) {
      set_error(result, "failed to create synchronization result");
    }
  }
  sg_plan_result_free(&plan);
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);
}

static void plan_sync_allowed_cb(struct mgp_list *arguments, struct mgp_graph *graph,
                                 struct mgp_result *result, struct mgp_memory *memory) {
  const char *model = NULL;
  int64_t budget = 0;
  if (!get_string_arg(arguments, 0U, &model) || !get_int_arg(arguments, 3U, &budget) ||
      budget <= 0 || (uint64_t)budget > (uint64_t)SIZE_MAX) {
    set_error(result, "expected model, nonempty hypotheses, allowed actions, and positive budget");
    return;
  }
  sg_automaton *automaton = NULL;
  sg_pair_oracle *oracle = NULL;
  if (!runtime_load(graph, memory, model, &automaton, &oracle, result)) {
    return;
  }
  size_t *hypotheses = NULL;
  size_t hypothesis_count = 0U;
  size_t *actions = NULL;
  size_t action_count = 0U;
  if (!argument_to_ids(automaton, arguments, 1U, false, false, &hypotheses, &hypothesis_count) ||
      !argument_to_ids(automaton, arguments, 2U, true, false, &actions, &action_count)) {
    set_error(result, "hypotheses or actions contain unknown prepared keys");
    free(hypotheses);
    free(actions);
    sg_pair_oracle_free(oracle);
    sg_automaton_free(automaton);
    return;
  }
  sg_plan_result plan = {0};
  const sg_status status = sg_plan_sync_allowed(automaton, oracle, hypotheses, hypothesis_count,
                                                actions, action_count, (size_t)budget, &plan);
  free(hypotheses);
  free(actions);
  if (status != SG_OK) {
    set_status_error(result, "restricted synchronization planning failed", status);
  } else {
    struct mgp_result_record *record = NULL;
    int64_t final_support = 0;
    const char *final_state = plan.final_state == SG_INDEX_NONE
                                  ? ""
                                  : sg_automaton_state_key(automaton, plan.final_state);
    if (final_state == NULL || !size_to_int64(plan.final_support_size, &final_support) ||
        !new_record(result, &record) || !insert_plan_common(record, automaton, &plan, memory) ||
        !insert_string(record, "final_state_key", final_state, memory) ||
        !insert_int(record, "final_support_size", final_support, memory)) {
      set_error(result, "failed to create restricted synchronization result");
    }
  }
  sg_plan_result_free(&plan);
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);
}

static void plan_goal_cb(struct mgp_list *arguments, struct mgp_graph *graph,
                         struct mgp_result *result, struct mgp_memory *memory) {
  const char *model = NULL;
  int64_t budget = 0;
  if (!get_string_arg(arguments, 0U, &model) || !get_int_arg(arguments, 4U, &budget) ||
      budget <= 0 || (uint64_t)budget > (uint64_t)SIZE_MAX) {
    set_error(result, "expected model, hypotheses, goals, actions, and positive budget");
    return;
  }
  sg_automaton *automaton = NULL;
  /* Automaton only: this procedure's sg_* call does not take the pair
   * table, and loading that reads 353,220 SyncPair rows. */
  if (!runtime_load_automaton(graph, memory, model, &automaton, result)) {
    return;
  }
  size_t *hypotheses = NULL;
  size_t hypothesis_count = 0U;
  size_t *goals = NULL;
  size_t goal_count = 0U;
  size_t *actions = NULL;
  size_t action_count = 0U;
  if (!argument_to_ids(automaton, arguments, 1U, false, false, &hypotheses, &hypothesis_count) ||
      !argument_to_ids(automaton, arguments, 2U, false, false, &goals, &goal_count) ||
      !argument_to_ids(automaton, arguments, 3U, true, false, &actions, &action_count)) {
    set_error(result, "hypotheses, goals, or actions contain unknown prepared keys");
    free(hypotheses);
    free(goals);
    free(actions);
    sg_automaton_free(automaton);
    return;
  }

  sg_plan_result plan = {0};
  const sg_status status = sg_plan_goal(automaton, hypotheses, hypothesis_count, goals, goal_count,
                                        actions, action_count, (size_t)budget, &plan);
  if (status != SG_OK) {
    set_status_error(result, "goal planning failed", status);
  } else {
    size_t *final_states = calloc(hypothesis_count, sizeof(*final_states));
    size_t final_count = 0U;
    sg_status apply_status = SG_OK;
    if (final_states == NULL) {
      apply_status = SG_ERR_ALLOC;
    } else if (plan.outcome == SG_OUTCOME_PLAN || plan.outcome == SG_OUTCOME_ALREADY_SATISFIED) {
      apply_status = sg_apply_word(automaton, hypotheses, hypothesis_count, &plan.word,
                                   final_states, &final_count);
    }
    struct mgp_result_record *record = NULL;
    int64_t final_support = 0;
    if (apply_status != SG_OK || !size_to_int64(plan.final_support_size, &final_support) ||
        !new_record(result, &record) || !insert_plan_common(record, automaton, &plan, memory) ||
        !insert_key_list(record, "final_state_keys", automaton, final_states, final_count, false,
                         memory) ||
        !insert_int(record, "final_support_size", final_support, memory)) {
      set_error(result, "failed to create goal-planning result");
    }
    free(final_states);
  }
  sg_plan_result_free(&plan);
  free(hypotheses);
  free(goals);
  free(actions);
  sg_automaton_free(automaton);
}

static void plan_disambiguate_cb(struct mgp_list *arguments, struct mgp_graph *graph,
                                 struct mgp_result *result, struct mgp_memory *memory) {
  const char *model = NULL;
  int64_t bound = 0;
  int64_t budget = 0;
  if (!get_string_arg(arguments, 0U, &model) || !get_int_arg(arguments, 2U, &bound) ||
      !get_int_arg(arguments, 4U, &budget) || bound <= 0 || budget <= 0 ||
      (uint64_t)bound > (uint64_t)SIZE_MAX || (uint64_t)budget > (uint64_t)SIZE_MAX) {
    set_error(result, "expected model, nonempty hypotheses, allowed actions, positive bound, and "
                      "positive budget");
    return;
  }
  sg_automaton *automaton = NULL;
  sg_pair_oracle *oracle = NULL;
  if (!runtime_load(graph, memory, model, &automaton, &oracle, result)) {
    return;
  }
  size_t *hypotheses = NULL;
  size_t hypothesis_count = 0U;
  if (!argument_to_ids(automaton, arguments, 1U, false, false, &hypotheses, &hypothesis_count)) {
    set_error(result, "hypotheses must be a nonempty list of prepared state keys");
    sg_pair_oracle_free(oracle);
    sg_automaton_free(automaton);
    return;
  }
  size_t *actions = NULL;
  size_t action_count = 0U;
  if (!argument_to_ids(automaton, arguments, 3U, true, false, &actions, &action_count)) {
    set_error(result, "actions must be a nonempty list of prepared action keys");
    free(hypotheses);
    sg_pair_oracle_free(oracle);
    sg_automaton_free(automaton);
    return;
  }
  sg_plan_result plan = {0};
  const sg_status status =
      sg_plan_disambiguate(automaton, oracle, hypotheses, hypothesis_count, (size_t)bound, actions,
                           action_count, (size_t)budget, &plan);
  free(actions);
  free(hypotheses);
  if (status != SG_OK) {
    set_status_error(result, "disambiguation planning failed", status);
  } else {
    struct mgp_result_record *record = NULL;
    int64_t best = 0;
    int64_t worst = 0;
    int64_t branches = 0;
    if (!size_to_int64(plan.best_support_size, &best) ||
        !size_to_int64(plan.worst_support_size, &worst) ||
        !size_to_int64(plan.branch_count, &branches) || !new_record(result, &record) ||
        !insert_plan_common(record, automaton, &plan, memory) ||
        !insert_int(record, "best_support_size", best, memory) ||
        !insert_int(record, "worst_support_size", worst, memory) ||
        !insert_int(record, "branch_count", branches, memory) ||
        !insert_bool(record, "homing", plan.homing, memory)) {
      set_error(result, "failed to create disambiguation result");
    }
  }
  sg_plan_result_free(&plan);
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);
}

static sg_status explain_visit(void *raw_context, size_t step, size_t action,
                               const size_t *predicted_states, size_t predicted_count,
                               const size_t *output_trace, size_t trace_length,
                               const size_t *branch_states, size_t branch_count) {
  explain_context *context = raw_context;
  struct mgp_result_record *record = NULL;
  int64_t step_value = 0;
  int64_t generation = 0;
  const char *action_key =
      action == SG_INDEX_NONE ? "" : sg_automaton_action_key(context->automaton, action);
  if (action_key == NULL || !size_to_int64(step, &step_value) ||
      !uint64_to_int64(sg_automaton_generation(context->automaton), &generation) ||
      !new_record(context->result, &record) ||
      !insert_int(record, "step", step_value, context->memory) ||
      !insert_string(record, "action", action_key, context->memory) ||
      !insert_key_list(record, "predicted_hypotheses", context->automaton, predicted_states,
                       predicted_count, false, context->memory) ||
      !insert_key_list(record, "output_trace", context->automaton, output_trace, trace_length, true,
                       context->memory) ||
      !insert_key_list(record, "branch_hypotheses", context->automaton, branch_states, branch_count,
                       false, context->memory) ||
      !insert_int(record, "generation", generation, context->memory)) {
    return SG_ERR_ALLOC;
  }
  return SG_OK;
}

static void explain_plan_cb(struct mgp_list *arguments, struct mgp_graph *graph,
                            struct mgp_result *result, struct mgp_memory *memory) {
  const char *model = NULL;
  int64_t generation = -1;
  if (!get_string_arg(arguments, 0U, &model) || !get_int_arg(arguments, 1U, &generation) ||
      generation < 0) {
    set_error(result, "expected model, generation, hypotheses, and word");
    return;
  }
  sg_automaton *automaton = NULL;
  /* Automaton only: this procedure's sg_* call does not take the pair
   * table, and loading that reads 353,220 SyncPair rows. */
  if (!runtime_load_automaton(graph, memory, model, &automaton, result)) {
    return;
  }
  size_t *hypotheses = NULL;
  size_t hypothesis_count = 0U;
  sg_word word = {0};
  if (!argument_to_ids(automaton, arguments, 2U, false, false, &hypotheses, &hypothesis_count) ||
      !argument_to_word(automaton, arguments, 3U, &word)) {
    set_error(result, "hypotheses or word contain unknown prepared keys");
    free(hypotheses);
    sg_automaton_free(automaton);
    return;
  }
  explain_context context = {
      .result = result,
      .automaton = automaton,
      .memory = memory,
  };
  const sg_status status = sg_explain_plan(automaton, (uint64_t)generation, hypotheses,
                                           hypothesis_count, &word, explain_visit, &context);
  if (status != SG_OK) {
    set_status_error(result, "plan explanation failed", status);
  }
  sg_word_free(&word);
  free(hypotheses);
  sg_automaton_free(automaton);
}

static bool insert_monitor_record(struct mgp_result *result, const sg_automaton *automaton,
                                  const sg_monitor_result *monitor, const char *status,
                                  const char *reason, struct mgp_memory *memory) {
  struct mgp_result_record *record = NULL;
  int64_t generation = 0;
  return uint64_to_int64(monitor->generation, &generation) && new_record(result, &record) &&
         insert_string(record, "status", status, memory) &&
         insert_string(record, "decision", sg_monitor_decision_name(monitor->decision), memory) &&
         insert_string(record, "reason", reason, memory) &&
         insert_key_list(record, "expected_hypotheses", automaton, monitor->expected_states,
                         monitor->expected_count, false, memory) &&
         insert_key_list(record, "unexpected_hypotheses", automaton, monitor->unexpected_states,
                         monitor->unexpected_count, false, memory) &&
         insert_int(record, "generation", generation, memory);
}

static bool insert_stale_monitor(struct mgp_result *result, uint64_t generation,
                                 struct mgp_memory *memory) {
  struct mgp_result_record *record = NULL;
  int64_t converted = 0;
  if (!uint64_to_int64(generation, &converted)) {
    return false;
  }
  return new_record(result, &record) && insert_string(record, "status", "OK", memory) &&
         insert_string(record, "decision", "STALE_GENERATION", memory) &&
         insert_string(record, "reason", "plan generation does not match model", memory) &&
         insert_empty_list(record, "expected_hypotheses", memory) &&
         insert_empty_list(record, "unexpected_hypotheses", memory) &&
         insert_int(record, "generation", converted, memory);
}

static const char *monitor_reason(sg_monitor_decision decision) {
  switch (decision) {
  case SG_MONITOR_CONTINUE:
    return "reported support matches prediction";
  case SG_MONITOR_REPLAN:
    return "reported support is a strict subset";
  case SG_MONITOR_MODEL_VIOLATION:
    return "reported support contains unexpected states";
  case SG_MONITOR_STALE_GENERATION:
    return "plan generation does not match model";
  case SG_MONITOR_WAIT:
    return "localizer report unavailable";
  }
  return "unknown monitor decision";
}

static void validate_update_cb(struct mgp_list *arguments, struct mgp_graph *graph,
                               struct mgp_result *result, struct mgp_memory *memory) {
  const char *model = NULL;
  int64_t plan_generation = -1;
  int64_t completed_steps = -1;
  bool localizer_available = true;
  if (!get_string_arg(arguments, 0U, &model) || !get_int_arg(arguments, 1U, &plan_generation) ||
      !get_int_arg(arguments, 4U, &completed_steps) || plan_generation < 0 || completed_steps < 0 ||
      !get_bool_arg_default(arguments, SG_VALIDATE_AVAILABLE_ARGUMENT, true,
                            &localizer_available)) {
    set_error(result, "invalid validation arguments");
    return;
  }
  model_metadata metadata = {0};
  sg_status status = load_metadata(graph, memory, model, &metadata);
  if (status != SG_OK) {
    set_status_error(result, "model metadata lookup failed", status);
    return;
  }
  if ((uint64_t)plan_generation != metadata.generation) {
    if (!insert_stale_monitor(result, metadata.generation, memory)) {
      set_error(result, "failed to create stale-generation result");
    }
    return;
  }
  sg_automaton *automaton = NULL;
  /* Automaton only: this procedure's sg_* call does not take the pair
   * table, and loading that reads 353,220 SyncPair rows. */
  if (!runtime_load_automaton(graph, memory, model, &automaton, result)) {
    return;
  }
  size_t *hypotheses = NULL;
  size_t hypothesis_count = 0U;
  size_t *reported = NULL;
  size_t reported_count = 0U;
  sg_word word = {0};
  if (!argument_to_ids(automaton, arguments, 2U, false, false, &hypotheses, &hypothesis_count) ||
      !argument_to_word(automaton, arguments, 3U, &word) ||
      !argument_to_ids(automaton, arguments, SG_VALIDATE_REPORTED_ARGUMENT, false,
                       !localizer_available, &reported, &reported_count) ||
      (uint64_t)completed_steps > (uint64_t)SIZE_MAX) {
    set_error(result, "hypotheses, word, step, or report is invalid for the prepared model");
    sg_word_free(&word);
    free(hypotheses);
    free(reported);
    sg_automaton_free(automaton);
    return;
  }
  sg_monitor_result monitor = {0};
  status = sg_validate_update(automaton, (uint64_t)plan_generation, hypotheses, hypothesis_count,
                              &word, (size_t)completed_steps, reported, reported_count,
                              localizer_available, &monitor);
  if (status != SG_OK) {
    set_status_error(result, "update validation failed", status);
  } else {
    const char *reason = monitor_reason(monitor.decision);
    if (!insert_monitor_record(result, automaton, &monitor, "OK", reason, memory)) {
      set_error(result, "failed to create update-validation result");
    }
  }
  sg_monitor_result_free(&monitor);
  sg_word_free(&word);
  free(hypotheses);
  free(reported);
  sg_automaton_free(automaton);
}

static void mark_dirty_cb(struct mgp_list *arguments, struct mgp_graph *graph,
                          struct mgp_result *result, struct mgp_memory *memory) {
  static const char *query = "MATCH (m:SyncModel {model: $model}) "
                             "SET m.dirty = true, m.generation = coalesce(m.generation, 0) + 1 "
                             "RETURN m.generation AS generation";
  const char *model = NULL;
  if (!get_string_arg(arguments, 0U, &model)) {
    set_error(result, "expected model");
    return;
  }
  struct mgp_map *params = make_model_params(model, memory);
  struct mgp_execution_result *execution = NULL;
  if (params == NULL || !mg_ok(mgp_execute_query(graph, memory, query, params, &execution)) ||
      execution == NULL) {
    if (params != NULL) {
      mgp_map_destroy(params);
    }
    set_error(result, "failed to mark model dirty");
    return;
  }
  struct mgp_map *row = NULL;
  int64_t generation = -1;
  if (!mg_ok(mgp_pull_one(execution, graph, memory, &row)) || row == NULL ||
      !row_int(row, "generation", &generation)) {
    set_error(result, "model was not found");
  } else {
    struct mgp_result_record *record = NULL;
    if (!new_record(result, &record) || !insert_string(record, "status", "DIRTY", memory) ||
        !insert_int(record, "generation", generation, memory)) {
      set_error(result, "failed to create dirty-model result");
    }
  }
  mgp_execution_result_destroy(execution);
  mgp_map_destroy(params);
}

static bool add_result(struct mgp_proc *procedure, const char *name, struct mgp_type *type) {
  return mg_ok(mgp_proc_add_result(procedure, name, type));
}

static bool add_required(struct mgp_proc *procedure, const char *name, struct mgp_type *type) {
  return mg_ok(mgp_proc_add_arg(procedure, name, type));
}

static bool add_optional_bool(struct mgp_proc *procedure, const char *name, bool default_value,
                              struct mgp_memory *memory, struct mgp_type *type) {
  struct mgp_value *value = NULL;
  if (!mg_ok(mgp_value_make_bool(default_value ? 1 : 0, memory, &value)) || value == NULL) {
    return false;
  }
  const bool ok = mg_ok(mgp_proc_add_opt_arg(procedure, name, type, value));
  mgp_value_destroy(value);
  return ok;
}

static bool register_prepare(struct mgp_module *module, struct mgp_memory *memory,
                             struct mgp_type *string_type, struct mgp_type *bool_type,
                             struct mgp_type *int_type) {
  struct mgp_proc *procedure = NULL;
  if (!mg_ok(
          mgp_module_add_write_procedure(module, "prepare_model", prepare_model_cb, &procedure)) ||
      procedure == NULL) {
    return false;
  }
  return add_required(procedure, "model", string_type) &&
         add_optional_bool(procedure, "materialize_pair_edges", false, memory, bool_type) &&
         add_result(procedure, "status", string_type) &&
         add_result(procedure, "generation", int_type) &&
         add_result(procedure, "states", int_type) && add_result(procedure, "actions", int_type) &&
         add_result(procedure, "outputs", int_type) &&
         add_result(procedure, "transitions", int_type) &&
         add_result(procedure, "pairs", int_type) &&
         add_result(procedure, "pair_edges", int_type) &&
         add_result(procedure, "mergeable_pairs", int_type) &&
         add_result(procedure, "resolvable_pairs", int_type) &&
         add_result(procedure, "materialized_pair_edges", bool_type);
}

static bool add_plan_common_results(struct mgp_proc *procedure, struct mgp_type *string_type,
                                    struct mgp_type *int_type, struct mgp_type *list_type) {
  return add_result(procedure, "status", string_type) &&
         add_result(procedure, "outcome", string_type) &&
         add_result(procedure, "reason", string_type) &&
         add_result(procedure, "method", string_type) && add_result(procedure, "word", list_type) &&
         add_result(procedure, "length", int_type) &&
         add_result(procedure, "expansions", int_type) &&
         add_result(procedure, "generation", int_type) &&
         add_result(procedure, "planning_time_us", int_type);
}

static bool register_plan_sync(struct mgp_module *module, struct mgp_type *string_type,
                               struct mgp_type *int_type, struct mgp_type *list_type) {
  struct mgp_proc *procedure = NULL;
  if (!mg_ok(mgp_module_add_read_procedure(module, "plan_sync", plan_sync_cb, &procedure)) ||
      procedure == NULL) {
    return false;
  }
  return add_required(procedure, "model", string_type) &&
         add_required(procedure, "hypotheses", list_type) &&
         add_required(procedure, "budget", int_type) &&
         add_plan_common_results(procedure, string_type, int_type, list_type) &&
         add_result(procedure, "final_state_key", string_type) &&
         add_result(procedure, "final_support_size", int_type);
}

static bool register_plan_sync_allowed(struct mgp_module *module, struct mgp_type *string_type,
                                       struct mgp_type *int_type, struct mgp_type *list_type) {
  struct mgp_proc *procedure = NULL;
  if (!mg_ok(mgp_module_add_read_procedure(module, "plan_sync_allowed", plan_sync_allowed_cb,
                                           &procedure)) ||
      procedure == NULL) {
    return false;
  }
  return add_required(procedure, "model", string_type) &&
         add_required(procedure, "hypotheses", list_type) &&
         add_required(procedure, "actions", list_type) &&
         add_required(procedure, "budget", int_type) &&
         add_plan_common_results(procedure, string_type, int_type, list_type) &&
         add_result(procedure, "final_state_key", string_type) &&
         add_result(procedure, "final_support_size", int_type);
}

static bool register_plan_goal(struct mgp_module *module, struct mgp_type *string_type,
                               struct mgp_type *int_type, struct mgp_type *list_type) {
  struct mgp_proc *procedure = NULL;
  if (!mg_ok(mgp_module_add_read_procedure(module, "plan_goal", plan_goal_cb, &procedure)) ||
      procedure == NULL) {
    return false;
  }
  return add_required(procedure, "model", string_type) &&
         add_required(procedure, "hypotheses", list_type) &&
         add_required(procedure, "goals", list_type) &&
         add_required(procedure, "actions", list_type) &&
         add_required(procedure, "budget", int_type) &&
         add_plan_common_results(procedure, string_type, int_type, list_type) &&
         add_result(procedure, "final_state_keys", list_type) &&
         add_result(procedure, "final_support_size", int_type);
}

static bool register_plan_disambiguate(struct mgp_module *module, struct mgp_type *string_type,
                                       struct mgp_type *bool_type, struct mgp_type *int_type,
                                       struct mgp_type *list_type) {
  struct mgp_proc *procedure = NULL;
  if (!mg_ok(mgp_module_add_read_procedure(module, "plan_disambiguate", plan_disambiguate_cb,
                                           &procedure)) ||
      procedure == NULL) {
    return false;
  }
  return add_required(procedure, "model", string_type) &&
         add_required(procedure, "hypotheses", list_type) &&
         add_required(procedure, "bound", int_type) &&
         add_required(procedure, "actions", list_type) &&
         add_required(procedure, "budget", int_type) &&
         add_plan_common_results(procedure, string_type, int_type, list_type) &&
         add_result(procedure, "best_support_size", int_type) &&
         add_result(procedure, "worst_support_size", int_type) &&
         add_result(procedure, "branch_count", int_type) &&
         add_result(procedure, "homing", bool_type);
}

static bool register_explain(struct mgp_module *module, struct mgp_type *string_type,
                             struct mgp_type *int_type, struct mgp_type *list_type) {
  struct mgp_proc *procedure = NULL;
  if (!mg_ok(mgp_module_add_read_procedure(module, "explain_plan", explain_plan_cb, &procedure)) ||
      procedure == NULL) {
    return false;
  }
  return add_required(procedure, "model", string_type) &&
         add_required(procedure, "generation", int_type) &&
         add_required(procedure, "hypotheses", list_type) &&
         add_required(procedure, "word", list_type) && add_result(procedure, "step", int_type) &&
         add_result(procedure, "action", string_type) &&
         add_result(procedure, "predicted_hypotheses", list_type) &&
         add_result(procedure, "output_trace", list_type) &&
         add_result(procedure, "branch_hypotheses", list_type) &&
         add_result(procedure, "generation", int_type);
}

static bool register_validate(struct mgp_module *module, struct mgp_memory *memory,
                              struct mgp_type *string_type, struct mgp_type *bool_type,
                              struct mgp_type *int_type, struct mgp_type *list_type) {
  struct mgp_proc *procedure = NULL;
  if (!mg_ok(mgp_module_add_read_procedure(module, "validate_update", validate_update_cb,
                                           &procedure)) ||
      procedure == NULL) {
    return false;
  }
  return add_required(procedure, "model", string_type) &&
         add_required(procedure, "generation", int_type) &&
         add_required(procedure, "hypotheses", list_type) &&
         add_required(procedure, "word", list_type) &&
         add_required(procedure, "completed_steps", int_type) &&
         add_required(procedure, "reported_hypotheses", list_type) &&
         add_optional_bool(procedure, "localizer_available", true, memory, bool_type) &&
         add_result(procedure, "status", string_type) &&
         add_result(procedure, "decision", string_type) &&
         add_result(procedure, "reason", string_type) &&
         add_result(procedure, "expected_hypotheses", list_type) &&
         add_result(procedure, "unexpected_hypotheses", list_type) &&
         add_result(procedure, "generation", int_type);
}

static bool register_mark_dirty(struct mgp_module *module, struct mgp_type *string_type,
                                struct mgp_type *int_type) {
  struct mgp_proc *procedure = NULL;
  if (!mg_ok(mgp_module_add_write_procedure(module, "mark_dirty", mark_dirty_cb, &procedure)) ||
      procedure == NULL) {
    return false;
  }
  return add_required(procedure, "model", string_type) &&
         add_result(procedure, "status", string_type) &&
         add_result(procedure, "generation", int_type);
}

int mgp_init_module(struct mgp_module *module, struct mgp_memory *memory) {
  struct mgp_type *string_type = NULL;
  struct mgp_type *bool_type = NULL;
  struct mgp_type *int_type = NULL;
  struct mgp_type *list_type = NULL;
  if (!mg_ok(mgp_type_string(&string_type)) || !mg_ok(mgp_type_bool(&bool_type)) ||
      !mg_ok(mgp_type_int(&int_type)) || !mg_ok(mgp_type_list(string_type, &list_type))) {
    return 1;
  }
  if (!register_prepare(module, memory, string_type, bool_type, int_type) ||
      !register_plan_sync(module, string_type, int_type, list_type) ||
      !register_plan_sync_allowed(module, string_type, int_type, list_type) ||
      !register_plan_goal(module, string_type, int_type, list_type) ||
      !register_plan_disambiguate(module, string_type, bool_type, int_type, list_type) ||
      !register_explain(module, string_type, int_type, list_type) ||
      !register_validate(module, memory, string_type, bool_type, int_type, list_type) ||
      !register_mark_dirty(module, string_type, int_type)) {
    return 1;
  }
  return 0;
}

int mgp_shutdown_module(void) {
  return 0;
}
