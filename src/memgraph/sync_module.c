#include "sync_kgraph/sync.h"

#include "mg_procedure.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mgp_init_module(struct mgp_module *module, struct mgp_memory *memory);
int mgp_shutdown_module(void);

static bool mg_ok(enum mgp_error error) {
  return error == MGP_ERROR_NO_ERROR;
}

#ifdef SYNC_KGRAPH_MGP_COMPAT
// Compat for Memgraph headers that predate the *_move API and
// mgp_unordered_map_make_empty. The non-move variants copy the value, so
// these shims destroy the source on success to preserve the move-semantics
// contract at the call sites (call sites destroy the value themselves on
// failure).
static enum mgp_error sync_compat_list_append_move(struct mgp_list *list,
                                                   struct mgp_value *value) {
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

static bool insert_value(struct mgp_result_record *record, const char *field,
                         struct mgp_value *value) {
  const bool ok = mg_ok(mgp_result_record_insert(record, field, value));
  mgp_value_destroy(value);
  return ok;
}

static bool insert_string(struct mgp_result_record *record, const char *field, const char *value,
                          struct mgp_memory *memory) {
  struct mgp_value *mg_value = NULL;
  if (!mg_ok(mgp_value_make_string(value, memory, &mg_value))) {
    return false;
  }
  return insert_value(record, field, mg_value);
}

static bool insert_int(struct mgp_result_record *record, const char *field, int64_t value,
                       struct mgp_memory *memory) {
  struct mgp_value *mg_value = NULL;
  if (!mg_ok(mgp_value_make_int(value, memory, &mg_value))) {
    return false;
  }
  return insert_value(record, field, mg_value);
}

static bool insert_bool(struct mgp_result_record *record, const char *field, bool value,
                        struct mgp_memory *memory) {
  struct mgp_value *mg_value = NULL;
  if (!mg_ok(mgp_value_make_bool(value ? 1 : 0, memory, &mg_value))) {
    return false;
  }
  return insert_value(record, field, mg_value);
}

static bool get_arg(struct mgp_list *args, size_t index, struct mgp_value **value) {
  size_t size = 0U;
  if (!mg_ok(mgp_list_size(args, &size)) || index >= size) {
    return false;
  }
  return mg_ok(mgp_list_at(args, index, value)) && *value != NULL;
}

static bool get_arg_string(struct mgp_list *args, size_t index, const char **value) {
  struct mgp_value *arg = NULL;
  if (!get_arg(args, index, &arg)) {
    return false;
  }
  enum mgp_value_type type = MGP_VALUE_TYPE_NULL;
  if (!mg_ok(mgp_value_get_type(arg, &type)) || type != MGP_VALUE_TYPE_STRING) {
    return false;
  }
  return mg_ok(mgp_value_get_string(arg, value)) && *value != NULL;
}

static bool get_arg_int_default(struct mgp_list *args, size_t index, int64_t default_value,
                                int64_t *value) {
  struct mgp_value *arg = NULL;
  if (!get_arg(args, index, &arg)) {
    *value = default_value;
    return true;
  }
  enum mgp_value_type type = MGP_VALUE_TYPE_NULL;
  if (!mg_ok(mgp_value_get_type(arg, &type)) || type == MGP_VALUE_TYPE_NULL) {
    *value = default_value;
    return true;
  }
  if (type != MGP_VALUE_TYPE_INT) {
    return false;
  }
  return mg_ok(mgp_value_get_int(arg, value));
}

static bool get_arg_bool_default(struct mgp_list *args, size_t index, bool default_value,
                                 bool *value) {
  struct mgp_value *arg = NULL;
  if (!get_arg(args, index, &arg)) {
    *value = default_value;
    return true;
  }
  enum mgp_value_type type = MGP_VALUE_TYPE_NULL;
  if (!mg_ok(mgp_value_get_type(arg, &type)) || type == MGP_VALUE_TYPE_NULL) {
    *value = default_value;
    return true;
  }
  if (type != MGP_VALUE_TYPE_BOOL) {
    return false;
  }
  int raw = 0;
  if (!mg_ok(mgp_value_get_bool(arg, &raw))) {
    return false;
  }
  *value = raw != 0;
  return true;
}

static bool get_arg_string_default(struct mgp_list *args, size_t index, const char *default_value,
                                   const char **value) {
  struct mgp_value *arg = NULL;
  if (!get_arg(args, index, &arg)) {
    *value = default_value;
    return true;
  }
  enum mgp_value_type type = MGP_VALUE_TYPE_NULL;
  if (!mg_ok(mgp_value_get_type(arg, &type)) || type == MGP_VALUE_TYPE_NULL) {
    *value = default_value;
    return true;
  }
  if (type != MGP_VALUE_TYPE_STRING) {
    return false;
  }
  return mg_ok(mgp_value_get_string(arg, value)) && *value != NULL;
}

static bool list_to_ids(const sg_dfa *dfa, struct mgp_value *value, size_t **ids, size_t *count) {
  *ids = NULL;
  *count = 0U;

  enum mgp_value_type type = MGP_VALUE_TYPE_NULL;
  if (!mg_ok(mgp_value_get_type(value, &type)) || type == MGP_VALUE_TYPE_NULL) {
    return true;
  }
  if (type != MGP_VALUE_TYPE_LIST) {
    return false;
  }

  struct mgp_list *list = NULL;
  if (!mg_ok(mgp_value_get_list(value, &list)) || list == NULL) {
    return false;
  }
  size_t size = 0U;
  if (!mg_ok(mgp_list_size(list, &size))) {
    return false;
  }

  size_t *created = calloc(size == 0U ? 1U : size, sizeof(created[0]));
  if (created == NULL) {
    return false;
  }

  for (size_t i = 0U; i < size; ++i) {
    struct mgp_value *item = NULL;
    const char *key = NULL;
    if (!mg_ok(mgp_list_at(list, i, &item)) || item == NULL ||
        !mg_ok(mgp_value_get_string(item, &key)) || key == NULL ||
        sg_dfa_find_state(dfa, key, &created[i]) != SG_OK) {
      free(created);
      return false;
    }
  }

  *ids = created;
  *count = size;
  return true;
}

static bool word_value_to_ids(const sg_dfa *dfa, struct mgp_value *value, sg_word *word) {
  if (sg_word_init(word) != SG_OK) {
    return false;
  }
  enum mgp_value_type type = MGP_VALUE_TYPE_NULL;
  if (!mg_ok(mgp_value_get_type(value, &type)) || type != MGP_VALUE_TYPE_LIST) {
    return false;
  }
  struct mgp_list *list = NULL;
  if (!mg_ok(mgp_value_get_list(value, &list)) || list == NULL) {
    return false;
  }
  size_t size = 0U;
  if (!mg_ok(mgp_list_size(list, &size))) {
    return false;
  }
  for (size_t i = 0U; i < size; ++i) {
    struct mgp_value *item = NULL;
    const char *letter = NULL;
    size_t letter_id = 0U;
    if (!mg_ok(mgp_list_at(list, i, &item)) || item == NULL ||
        !mg_ok(mgp_value_get_string(item, &letter)) || letter == NULL ||
        sg_dfa_find_letter(dfa, letter, &letter_id) != SG_OK ||
        sg_word_append(word, letter_id) != SG_OK) {
      return false;
    }
  }
  return true;
}

static struct mgp_map *make_model_params(const char *model, struct mgp_memory *memory) {
  struct mgp_map *params = NULL;
  struct mgp_value *model_value = NULL;
  if (!mg_ok(mgp_unordered_map_make_empty(memory, &params)) || params == NULL) {
    return NULL;
  }
  if (!mg_ok(mgp_value_make_string(model, memory, &model_value)) || model_value == NULL) {
    mgp_map_destroy(params);
    return NULL;
  }
  if (!mg_ok(mgp_map_insert_move(params, "model", model_value))) {
    mgp_value_destroy(model_value);
    mgp_map_destroy(params);
    return NULL;
  }
  return params;
}

static bool params_insert_value(struct mgp_map *params, const char *key, struct mgp_value *value) {
  if (params == NULL || key == NULL || value == NULL) {
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
  struct mgp_value *mg_value = NULL;
  if (!mg_ok(mgp_value_make_string(value, memory, &mg_value)) || mg_value == NULL) {
    return false;
  }
  return params_insert_value(params, key, mg_value);
}

static bool params_insert_int(struct mgp_map *params, const char *key, int64_t value,
                              struct mgp_memory *memory) {
  struct mgp_value *mg_value = NULL;
  if (!mg_ok(mgp_value_make_int(value, memory, &mg_value)) || mg_value == NULL) {
    return false;
  }
  return params_insert_value(params, key, mg_value);
}

static bool params_insert_bool(struct mgp_map *params, const char *key, bool value,
                               struct mgp_memory *memory) {
  struct mgp_value *mg_value = NULL;
  if (!mg_ok(mgp_value_make_bool(value ? 1 : 0, memory, &mg_value)) || mg_value == NULL) {
    return false;
  }
  return params_insert_value(params, key, mg_value);
}

static bool exec_query_drain(struct mgp_graph *graph, struct mgp_memory *memory, const char *query,
                             struct mgp_map *params) {
  struct mgp_execution_result *exec = NULL;
  if (!mg_ok(mgp_execute_query(graph, memory, query, params, &exec)) || exec == NULL) {
    return false;
  }

  bool ok = true;
  for (;;) {
    struct mgp_map *row = NULL;
    if (!mg_ok(mgp_pull_one(exec, graph, memory, &row))) {
      ok = false;
      break;
    }
    if (row == NULL) {
      break;
    }
  }

  mgp_execution_result_destroy(exec);
  return ok;
}

static bool exec_model_query(struct mgp_graph *graph, struct mgp_memory *memory, const char *model,
                             const char *query) {
  struct mgp_map *params = make_model_params(model, memory);
  if (params == NULL) {
    return false;
  }
  const bool ok = exec_query_drain(graph, memory, query, params);
  mgp_map_destroy(params);
  return ok;
}

static int64_t size_to_int64(size_t value) {
  return (value > (size_t)INT64_MAX) ? INT64_MAX : (int64_t)value;
}

static char *join_state_keys(const sg_dfa *dfa, const size_t *states, size_t state_count) {
  if (dfa == NULL || (states == NULL && state_count != 0U)) {
    return NULL;
  }
  size_t length = 0U;
  for (size_t i = 0U; i < state_count; ++i) {
    const char *key = sg_dfa_state_key(dfa, states[i]);
    if (key == NULL) {
      return NULL;
    }
    const size_t add = strlen(key) + (i == 0U ? 0U : 1U);
    if (add > SIZE_MAX - length) {
      return NULL;
    }
    length += add;
  }
  if (length == SIZE_MAX) {
    return NULL;
  }

  char *joined = malloc(length + 1U);
  if (joined == NULL) {
    return NULL;
  }
  char *cursor = joined;
  for (size_t i = 0U; i < state_count; ++i) {
    const char *key = sg_dfa_state_key(dfa, states[i]);
    if (i != 0U) {
      *cursor = ',';
      ++cursor;
    }
    const size_t key_len = strlen(key);
    memcpy(cursor, key, key_len);
    cursor += key_len;
  }
  *cursor = '\0';
  return joined;
}

static char *join_word_letters(const sg_dfa *dfa, const sg_word *word) {
  if (dfa == NULL || word == NULL) {
    return NULL;
  }
  size_t length = 0U;
  for (size_t i = 0U; i < word->length; ++i) {
    const char *key = sg_dfa_letter_key(dfa, word->letters[i]);
    if (key == NULL) {
      return NULL;
    }
    const size_t add = strlen(key) + (i == 0U ? 0U : 1U);
    if (add > SIZE_MAX - length) {
      return NULL;
    }
    length += add;
  }
  if (length == SIZE_MAX) {
    return NULL;
  }

  char *joined = malloc(length + 1U);
  if (joined == NULL) {
    return NULL;
  }
  char *cursor = joined;
  for (size_t i = 0U; i < word->length; ++i) {
    const char *key = sg_dfa_letter_key(dfa, word->letters[i]);
    if (i != 0U) {
      *cursor = ',';
      ++cursor;
    }
    const size_t key_len = strlen(key);
    memcpy(cursor, key, key_len);
    cursor += key_len;
  }
  *cursor = '\0';
  return joined;
}

static bool row_string(struct mgp_map *row, const char *field, const char **value) {
  struct mgp_value *field_value = NULL;
  if (!mg_ok(mgp_map_at(row, field, &field_value)) || field_value == NULL) {
    return false;
  }
  return mg_ok(mgp_value_get_string(field_value, value)) && *value != NULL;
}

static bool exec_add_single_field(struct mgp_graph *graph, struct mgp_memory *memory,
                                  const char *model, const char *query, const char *field,
                                  sg_dfa_builder *builder, bool add_state) {
  struct mgp_map *params = make_model_params(model, memory);
  if (params == NULL) {
    return false;
  }
  struct mgp_execution_result *exec = NULL;
  if (!mg_ok(mgp_execute_query(graph, memory, query, params, &exec)) || exec == NULL) {
    mgp_map_destroy(params);
    return false;
  }

  bool ok = true;
  for (;;) {
    struct mgp_map *row = NULL;
    if (!mg_ok(mgp_pull_one(exec, graph, memory, &row))) {
      ok = false;
      break;
    }
    if (row == NULL) {
      break;
    }
    const char *value = NULL;
    if (!row_string(row, field, &value)) {
      ok = false;
      break;
    }
    sg_status status = add_state ? sg_dfa_builder_add_state(builder, value)
                                 : sg_dfa_builder_add_letter(builder, value);
    if (status != SG_OK) {
      ok = false;
      break;
    }
  }

  mgp_execution_result_destroy(exec);
  mgp_map_destroy(params);
  return ok;
}

static bool exec_add_transitions(struct mgp_graph *graph, struct mgp_memory *memory,
                                 const char *model, sg_dfa_builder *builder) {
  static const char *query =
      "MATCH (src:SyncState {model: $model})-[t:SYNC_TRANS {model: $model}]->"
      "(dst:SyncState {model: $model}) "
      "RETURN src.state_key AS source, t.letter AS letter, dst.state_key AS target "
      "ORDER BY source, letter, target";
  struct mgp_map *params = make_model_params(model, memory);
  if (params == NULL) {
    return false;
  }
  struct mgp_execution_result *exec = NULL;
  if (!mg_ok(mgp_execute_query(graph, memory, query, params, &exec)) || exec == NULL) {
    mgp_map_destroy(params);
    return false;
  }

  bool ok = true;
  for (;;) {
    struct mgp_map *row = NULL;
    if (!mg_ok(mgp_pull_one(exec, graph, memory, &row))) {
      ok = false;
      break;
    }
    if (row == NULL) {
      break;
    }
    const char *source = NULL;
    const char *letter = NULL;
    const char *target = NULL;
    if (!row_string(row, "source", &source) || !row_string(row, "letter", &letter) ||
        !row_string(row, "target", &target) ||
        sg_dfa_builder_add_transition(builder, source, letter, target) != SG_OK) {
      ok = false;
      break;
    }
  }

  mgp_execution_result_destroy(exec);
  mgp_map_destroy(params);
  return ok;
}

static sg_status load_model(struct mgp_graph *graph, struct mgp_memory *memory, const char *model,
                            bool complete_with_sink, sg_dfa **dfa) {
  static const char *state_query =
      "MATCH (s:SyncState {model: $model}) RETURN s.state_key AS state_key "
      "ORDER BY s.state_id, s.state_key";
  static const char *letter_query =
      "MATCH (l:SyncLetter {model: $model}) RETURN l.letter AS letter "
      "ORDER BY l.letter_id, l.letter";

  sg_dfa_builder *builder = NULL;
  sg_status status = sg_dfa_builder_init(&builder);
  if (status != SG_OK) {
    return status;
  }
  if (!exec_add_single_field(graph, memory, model, state_query, "state_key", builder, true) ||
      !exec_add_single_field(graph, memory, model, letter_query, "letter", builder, false) ||
      !exec_add_transitions(graph, memory, model, builder)) {
    sg_dfa_builder_free(builder);
    return SG_ERR_INVALID_ARGUMENT;
  }

  status = sg_dfa_builder_build(builder, complete_with_sink, dfa);
  sg_dfa_builder_free(builder);
  return status;
}

static bool insert_word(struct mgp_result_record *record, const sg_dfa *dfa, const sg_word *word,
                        struct mgp_memory *memory) {
  struct mgp_list *list = NULL;
  if (!mg_ok(mgp_list_make_empty(word->length, memory, &list)) || list == NULL) {
    return false;
  }
  for (size_t i = 0U; i < word->length; ++i) {
    struct mgp_value *value = NULL;
    if (!mg_ok(mgp_value_make_string(sg_dfa_letter_key(dfa, word->letters[i]), memory, &value)) ||
        value == NULL) {
      mgp_list_destroy(list);
      return false;
    }
    if (!mg_ok(mgp_list_append_move(list, value))) {
      mgp_value_destroy(value);
      mgp_list_destroy(list);
      return false;
    }
  }
  struct mgp_value *list_value = NULL;
  if (!mg_ok(mgp_value_make_list(list, &list_value)) || list_value == NULL) {
    mgp_list_destroy(list);
    return false;
  }
  return insert_value(record, "word", list_value);
}

static bool new_record(struct mgp_result *result, struct mgp_result_record **record) {
  return mg_ok(mgp_result_new_record(result, record)) && *record != NULL;
}

static bool create_pair_node(struct mgp_graph *graph, struct mgp_memory *memory, const char *model,
                             const sg_dfa *dfa, const sg_pair_oracle *oracle, size_t pair) {
  static const char *query =
      "CREATE (:SyncPair {model: $model, pair_id: $pair_id, first_key: $first_key, "
      "second_key: $second_key, distance: $distance, has_witness: $has_witness, "
      "witness: $witness, next_pair: $next_pair, generation: 0}) "
      "RETURN 1 AS ok";

  size_t first = 0U;
  size_t second = 0U;
  bool has_witness = false;
  size_t distance = 0U;
  size_t witness_letter = 0U;
  size_t next_pair = 0U;
  if (sg_pair_oracle_pair_states(oracle, pair, &first, &second) != SG_OK ||
      sg_pair_oracle_pair_witness(oracle, pair, &has_witness, &distance, &witness_letter,
                                  &next_pair) != SG_OK) {
    return false;
  }

  const char *first_key = sg_dfa_state_key(dfa, first);
  const char *second_key = sg_dfa_state_key(dfa, second);
  const char *witness = (has_witness && witness_letter < sg_dfa_letter_count(dfa))
                            ? sg_dfa_letter_key(dfa, witness_letter)
                            : "";
  const int64_t distance_value = has_witness ? size_to_int64(distance) : -1;
  const int64_t next_pair_value = (has_witness && next_pair < sg_pair_oracle_pair_count(oracle))
                                      ? size_to_int64(next_pair)
                                      : -1;

  struct mgp_map *params = make_model_params(model, memory);
  if (params == NULL) {
    return false;
  }
  const bool ok = params_insert_int(params, "pair_id", size_to_int64(pair), memory) &&
                  params_insert_string(params, "first_key", first_key, memory) &&
                  params_insert_string(params, "second_key", second_key, memory) &&
                  params_insert_int(params, "distance", distance_value, memory) &&
                  params_insert_bool(params, "has_witness", has_witness, memory) &&
                  params_insert_string(params, "witness", witness, memory) &&
                  params_insert_int(params, "next_pair", next_pair_value, memory) &&
                  exec_query_drain(graph, memory, query, params);
  mgp_map_destroy(params);
  return ok;
}

static bool create_pair_edge(struct mgp_graph *graph, struct mgp_memory *memory, const char *model,
                             const sg_dfa *dfa, const sg_pair_oracle *oracle, size_t pair,
                             size_t letter) {
  static const char *query =
      "MATCH (p:SyncPair {model: $model, pair_id: $pair_id}), "
      "(n:SyncPair {model: $model, pair_id: $next_pair}) "
      "CREATE (p)-[:PAIR_NEXT {model: $model, letter: $letter, letter_id: $letter_id}]->(n) "
      "CREATE (n)-[:PAIR_PRE {model: $model, letter: $letter, letter_id: $letter_id}]->(p) "
      "RETURN 1 AS ok";

  size_t next_pair = 0U;
  if (sg_pair_oracle_pair_next(oracle, pair, letter, &next_pair) != SG_OK) {
    return false;
  }
  const char *letter_key = sg_dfa_letter_key(dfa, letter);
  if (letter_key == NULL) {
    return false;
  }

  struct mgp_map *params = make_model_params(model, memory);
  if (params == NULL) {
    return false;
  }
  const bool ok = params_insert_int(params, "pair_id", size_to_int64(pair), memory) &&
                  params_insert_int(params, "next_pair", size_to_int64(next_pair), memory) &&
                  params_insert_string(params, "letter", letter_key, memory) &&
                  params_insert_int(params, "letter_id", size_to_int64(letter), memory) &&
                  exec_query_drain(graph, memory, query, params);
  mgp_map_destroy(params);
  return ok;
}

static bool materialize_pair_oracle(struct mgp_graph *graph, struct mgp_memory *memory,
                                    const char *model, const sg_dfa *dfa,
                                    const sg_pair_oracle *oracle) {
  static const char *clear_query =
      "MATCH (p:SyncPair {model: $model}) DETACH DELETE p RETURN count(p) AS removed";
  if (!exec_model_query(graph, memory, model, clear_query)) {
    return false;
  }

  const size_t pair_count = sg_pair_oracle_pair_count(oracle);
  const size_t letter_count = sg_dfa_letter_count(dfa);
  for (size_t pair = 0U; pair < pair_count; ++pair) {
    if (!create_pair_node(graph, memory, model, dfa, oracle, pair)) {
      return false;
    }
  }
  for (size_t pair = 0U; pair < pair_count; ++pair) {
    for (size_t letter = 0U; letter < letter_count; ++letter) {
      if (!create_pair_edge(graph, memory, model, dfa, oracle, pair, letter)) {
        return false;
      }
    }
  }
  return true;
}

typedef struct {
  struct mgp_graph *graph;
  struct mgp_memory *memory;
  const char *model;
  const sg_dfa *dfa;
  const char *mode;
  const char *target_key;
} subset_materialize_ctx;

static bool clear_subset_cache(struct mgp_graph *graph, struct mgp_memory *memory,
                               const char *model, const char *mode, const char *target_key) {
  static const char *query =
      "MATCH (s:SyncSubset {model: $model, mode: $mode, target_key: $target_key}) "
      "DETACH DELETE s RETURN count(s) AS removed";
  struct mgp_map *params = make_model_params(model, memory);
  if (params == NULL) {
    return false;
  }
  const bool ok = params_insert_string(params, "mode", mode, memory) &&
                  params_insert_string(params, "target_key", target_key, memory) &&
                  exec_query_drain(graph, memory, query, params);
  mgp_map_destroy(params);
  return ok;
}

static bool create_subset_node(const subset_materialize_ctx *ctx, const size_t *states,
                               size_t state_count, const sg_word *word) {
  static const char *query =
      "MERGE (s:SyncSubset {model: $model, mode: $mode, target_key: $target_key, "
      "subset_key: $subset_key}) "
      "SET s.word = $word, s.size = $size, s.word_length = $word_length, s.generation = 0 "
      "RETURN 1 AS ok";

  char *subset_key = join_state_keys(ctx->dfa, states, state_count);
  char *word_key = join_word_letters(ctx->dfa, word);
  if (subset_key == NULL || word_key == NULL) {
    free(subset_key);
    free(word_key);
    return false;
  }

  struct mgp_map *params = make_model_params(ctx->model, ctx->memory);
  if (params == NULL) {
    free(subset_key);
    free(word_key);
    return false;
  }
  const bool ok =
      params_insert_string(params, "mode", ctx->mode, ctx->memory) &&
      params_insert_string(params, "target_key", ctx->target_key, ctx->memory) &&
      params_insert_string(params, "subset_key", subset_key, ctx->memory) &&
      params_insert_string(params, "word", word_key, ctx->memory) &&
      params_insert_int(params, "size", size_to_int64(state_count), ctx->memory) &&
      params_insert_int(params, "word_length", size_to_int64(word->length), ctx->memory) &&
      exec_query_drain(ctx->graph, ctx->memory, query, params);
  mgp_map_destroy(params);
  free(subset_key);
  free(word_key);
  return ok;
}

static sg_status materialize_subset_visit(void *raw_ctx, const size_t *states, size_t state_count,
                                          const sg_word *word) {
  const subset_materialize_ctx *ctx = raw_ctx;
  if (ctx == NULL || word == NULL || (states == NULL && state_count != 0U)) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  return create_subset_node(ctx, states, state_count, word) ? SG_OK : SG_ERR_INVALID_ARGUMENT;
}

static void validate_model_cb(struct mgp_list *args, struct mgp_graph *graph,
                              struct mgp_result *result, struct mgp_memory *memory) {
  const char *model = NULL;
  if (!get_arg_string(args, 0U, &model)) {
    set_error(result, "validate_model requires a string model argument");
    return;
  }
  sg_dfa *dfa = NULL;
  const sg_status status = load_model(graph, memory, model, false, &dfa);

  struct mgp_result_record *record = NULL;
  if (!new_record(result, &record)) {
    sg_dfa_free(dfa);
    set_error(result, "failed to create result record");
    return;
  }

  const bool ok = status == SG_OK;
  if (!insert_bool(record, "ok", ok, memory) ||
      !insert_string(record, "code", sg_status_name(status), memory) ||
      !insert_string(record, "message",
                     ok ? "model is complete and deterministic" : sg_status_name(status), memory) ||
      !insert_int(record, "states", ok ? (int64_t)sg_dfa_state_count(dfa) : 0, memory) ||
      !insert_int(record, "letters", ok ? (int64_t)sg_dfa_letter_count(dfa) : 0, memory) ||
      !insert_int(record, "transitions", ok ? (int64_t)sg_dfa_transition_count(dfa) : 0, memory)) {
    sg_dfa_free(dfa);
    set_error(result, "failed to insert validation result");
    return;
  }
  sg_dfa_free(dfa);
}

static void build_model_cb(struct mgp_list *args, struct mgp_graph *graph,
                           struct mgp_result *result, struct mgp_memory *memory) {
  const char *model = NULL;
  bool complete_with_sink = false;
  if (!get_arg_string(args, 0U, &model) ||
      !get_arg_bool_default(args, 1U, false, &complete_with_sink)) {
    set_error(result, "build_model requires model and optional complete_with_sink");
    return;
  }
  sg_dfa *dfa = NULL;
  const sg_status status = load_model(graph, memory, model, complete_with_sink, &dfa);
  struct mgp_result_record *record = NULL;
  if (!new_record(result, &record)) {
    sg_dfa_free(dfa);
    set_error(result, "failed to create result record");
    return;
  }
  const bool ok = status == SG_OK;
  if (!insert_string(record, "status", sg_status_name(status), memory) ||
      !insert_int(record, "generation", 0, memory) ||
      !insert_int(record, "states", ok ? (int64_t)sg_dfa_state_count(dfa) : 0, memory) ||
      !insert_int(record, "letters", ok ? (int64_t)sg_dfa_letter_count(dfa) : 0, memory) ||
      !insert_int(record, "transitions", ok ? (int64_t)sg_dfa_transition_count(dfa) : 0, memory)) {
    sg_dfa_free(dfa);
    set_error(result, "failed to insert build result");
    return;
  }
  sg_dfa_free(dfa);
}

static void build_pair_oracle_cb(struct mgp_list *args, struct mgp_graph *graph,
                                 struct mgp_result *result, struct mgp_memory *memory) {
  const char *model = NULL;
  bool materialize = true;
  if (!get_arg_string(args, 0U, &model) || !get_arg_bool_default(args, 1U, true, &materialize)) {
    set_error(result, "build_pair_oracle requires model and optional materialize flag");
    return;
  }
  sg_dfa *dfa = NULL;
  sg_pair_oracle *oracle = NULL;
  sg_status status = load_model(graph, memory, model, false, &dfa);
  if (status == SG_OK) {
    status = sg_pair_oracle_build(dfa, &oracle);
  }
  if (status == SG_OK && materialize &&
      !materialize_pair_oracle(graph, memory, model, dfa, oracle)) {
    status = SG_ERR_INVALID_ARGUMENT;
  }

  struct mgp_result_record *record = NULL;
  if (!new_record(result, &record)) {
    sg_pair_oracle_free(oracle);
    sg_dfa_free(dfa);
    set_error(result, "failed to create result record");
    return;
  }
  if (!insert_string(record, "status", sg_status_name(status), memory) ||
      !insert_int(record, "pairs", oracle == NULL ? 0 : (int64_t)sg_pair_oracle_pair_count(oracle),
                  memory) ||
      !insert_int(record, "pair_edges",
                  oracle == NULL ? 0 : (int64_t)sg_pair_oracle_pair_edge_count(oracle), memory) ||
      !insert_int(record, "mergeable_pairs",
                  oracle == NULL ? 0 : (int64_t)sg_pair_oracle_mergeable_pair_count(oracle),
                  memory) ||
      !insert_bool(record, "materialized", status == SG_OK && materialize, memory) ||
      !insert_int(record, "generation", 0, memory)) {
    sg_pair_oracle_free(oracle);
    sg_dfa_free(dfa);
    set_error(result, "failed to insert oracle result");
    return;
  }
  sg_pair_oracle_free(oracle);
  sg_dfa_free(dfa);
}

static void word_for_set_impl(struct mgp_list *args, struct mgp_graph *graph,
                              struct mgp_result *result, struct mgp_memory *memory,
                              bool target_required) {
  const char *model = NULL;
  const char *mode_name = target_required ? "REACH_AND_SYNC" : "SYNC";
  int64_t budget = 0;
  if (!get_arg_string(args, 0U, &model) ||
      !get_arg_string_default(args, 3U, mode_name, &mode_name) ||
      !get_arg_int_default(args, 4U, 0, &budget)) {
    set_error(result, "invalid word_for_set arguments");
    return;
  }
  sg_mode mode = SG_MODE_SYNC;
  if (sg_mode_parse(mode_name, &mode) != SG_OK || budget < 0) {
    set_error(result, "invalid mode or budget");
    return;
  }

  sg_dfa *dfa = NULL;
  sg_pair_oracle *oracle = NULL;
  sg_status status = load_model(graph, memory, model, false, &dfa);
  if (status == SG_OK) {
    status = sg_pair_oracle_build(dfa, &oracle);
  }
  if (status != SG_OK) {
    struct mgp_result_record *record = NULL;
    if (new_record(result, &record)) {
      (void)insert_string(record, "status", sg_status_name(status), memory);
      (void)insert_int(record, "length", 0, memory);
      (void)insert_string(record, "final_state_key", "", memory);
      sg_word empty = {0};
      (void)sg_word_init(&empty);
      (void)insert_word(record, dfa, &empty, memory);
    }
    sg_pair_oracle_free(oracle);
    sg_dfa_free(dfa);
    return;
  }

  struct mgp_value *state_arg = NULL;
  struct mgp_value *target_arg = NULL;
  if (!get_arg(args, 1U, &state_arg) || !get_arg(args, 2U, &target_arg)) {
    set_error(result, "state_keys and target_keys list arguments are required");
    sg_pair_oracle_free(oracle);
    sg_dfa_free(dfa);
    return;
  }

  size_t *states = NULL;
  size_t *targets = NULL;
  size_t state_count = 0U;
  size_t target_count = 0U;
  if (!list_to_ids(dfa, state_arg, &states, &state_count) ||
      !list_to_ids(dfa, target_arg, &targets, &target_count) ||
      (target_required && target_count == 0U)) {
    free(states);
    free(targets);
    set_error(result, "state_keys and target_keys must be lists of known state keys");
    sg_pair_oracle_free(oracle);
    sg_dfa_free(dfa);
    return;
  }

  sg_word_result word_result = {0};
  status = sg_word_for_set(dfa, oracle, states, state_count, targets, target_count, mode,
                           (size_t)budget, &word_result);

  struct mgp_result_record *record = NULL;
  if (!new_record(result, &record) ||
      !insert_string(record, "status",
                     status == SG_OK ? sg_result_kind_name(word_result.kind)
                                     : sg_status_name(status),
                     memory) ||
      !insert_word(record, dfa, &word_result.word, memory) ||
      !insert_int(record, "length", (int64_t)word_result.word.length, memory) ||
      !insert_string(record, "final_state_key",
                     word_result.final_state == (size_t)-1
                         ? ""
                         : sg_dfa_state_key(dfa, word_result.final_state),
                     memory) ||
      !insert_int(record, "generation", 0, memory)) {
    set_error(result, "failed to insert word result");
  }

  sg_word_result_free(&word_result);
  free(states);
  free(targets);
  sg_pair_oracle_free(oracle);
  sg_dfa_free(dfa);
}

static void word_for_set_cb(struct mgp_list *args, struct mgp_graph *graph,
                            struct mgp_result *result, struct mgp_memory *memory) {
  word_for_set_impl(args, graph, result, memory, false);
}

static void word_to_target_cb(struct mgp_list *args, struct mgp_graph *graph,
                              struct mgp_result *result, struct mgp_memory *memory) {
  word_for_set_impl(args, graph, result, memory, true);
}

static void expand_cache_cb(struct mgp_list *args, struct mgp_graph *graph,
                            struct mgp_result *result, struct mgp_memory *memory) {
  const char *model = NULL;
  const char *mode_name = "REACH_AND_SYNC";
  int64_t budget = 0;
  if (!get_arg_string(args, 0U, &model) ||
      !get_arg_string_default(args, 2U, mode_name, &mode_name) ||
      !get_arg_int_default(args, 3U, 0, &budget) || budget < 0) {
    set_error(result, "expand_cache requires model, target_keys, optional mode, optional budget");
    return;
  }

  sg_mode mode = SG_MODE_REACH_AND_SYNC;
  if (sg_mode_parse(mode_name, &mode) != SG_OK) {
    set_error(result, "invalid expand_cache mode");
    return;
  }

  sg_dfa *dfa = NULL;
  sg_status status = load_model(graph, memory, model, false, &dfa);
  if (status != SG_OK) {
    struct mgp_result_record *record = NULL;
    if (new_record(result, &record)) {
      (void)insert_string(record, "status", sg_status_name(status), memory);
      (void)insert_int(record, "expanded", 0, memory);
      (void)insert_int(record, "cache_size", 0, memory);
    }
    return;
  }

  struct mgp_value *target_arg = NULL;
  size_t *targets = NULL;
  size_t target_count = 0U;
  if (!get_arg(args, 1U, &target_arg) || !list_to_ids(dfa, target_arg, &targets, &target_count)) {
    free(targets);
    sg_dfa_free(dfa);
    set_error(result, "target_keys must be a list of known state keys");
    return;
  }

  const char *canonical_mode = sg_mode_name(mode);
  char *target_key = join_state_keys(dfa, targets, target_count);
  if (target_key == NULL) {
    free(targets);
    sg_dfa_free(dfa);
    set_error(result, "failed to build target cache key");
    return;
  }
  if (!clear_subset_cache(graph, memory, model, canonical_mode, target_key)) {
    free(target_key);
    free(targets);
    sg_dfa_free(dfa);
    set_error(result, "failed to clear subset cache");
    return;
  }

  subset_materialize_ctx ctx = {
      .graph = graph,
      .memory = memory,
      .model = model,
      .dfa = dfa,
      .mode = canonical_mode,
      .target_key = target_key,
  };
  size_t expanded = 0U;
  size_t cache_size = 0U;
  status = sg_expand_cache_visit(dfa, targets, target_count, mode, (size_t)budget,
                                 materialize_subset_visit, &ctx, &expanded, &cache_size);

  struct mgp_result_record *record = NULL;
  if (!new_record(result, &record) ||
      !insert_string(record, "status", sg_status_name(status), memory) ||
      !insert_int(record, "expanded", (int64_t)expanded, memory) ||
      !insert_int(record, "cache_size", (int64_t)cache_size, memory)) {
    set_error(result, "failed to insert cache result");
  }
  free(target_key);
  free(targets);
  sg_dfa_free(dfa);
}

static void explain_cb(struct mgp_list *args, struct mgp_graph *graph, struct mgp_result *result,
                       struct mgp_memory *memory) {
  const char *model = NULL;
  if (!get_arg_string(args, 0U, &model)) {
    set_error(result, "explain requires model, state_keys, word");
    return;
  }
  sg_dfa *dfa = NULL;
  sg_status status = load_model(graph, memory, model, false, &dfa);
  if (status != SG_OK) {
    set_error(result, sg_status_name(status));
    return;
  }

  struct mgp_value *state_arg = NULL;
  struct mgp_value *word_arg = NULL;
  size_t *states = NULL;
  size_t state_count = 0U;
  sg_word word = {0};
  if (!get_arg(args, 1U, &state_arg) || !get_arg(args, 2U, &word_arg) ||
      !list_to_ids(dfa, state_arg, &states, &state_count) ||
      !word_value_to_ids(dfa, word_arg, &word)) {
    free(states);
    sg_word_free(&word);
    sg_dfa_free(dfa);
    set_error(result, "state_keys and word must be lists of known keys");
    return;
  }

  size_t *steps = NULL;
  size_t *counts = NULL;
  size_t step_count = 0U;
  status = sg_explain_word(dfa, states, state_count, &word, &steps, &counts, &step_count);
  if (status != SG_OK) {
    free(states);
    sg_word_free(&word);
    sg_dfa_free(dfa);
    set_error(result, sg_status_name(status));
    return;
  }

  for (size_t step = 0U; step < step_count; ++step) {
    struct mgp_result_record *record = NULL;
    if (!new_record(result, &record) || !insert_int(record, "step", (int64_t)step, memory)) {
      set_error(result, "failed to insert explain record");
      break;
    }

    const char *letter = "";
    if (step > 0U) {
      letter = sg_dfa_letter_key(dfa, word.letters[step - 1U]);
    }
    if (!insert_string(record, "letter", letter, memory)) {
      set_error(result, "failed to insert explain letter");
      break;
    }

    struct mgp_list *active = NULL;
    if (!mg_ok(mgp_list_make_empty(counts[step], memory, &active)) || active == NULL) {
      set_error(result, "failed to create active list");
      break;
    }
    for (size_t i = 0U; i < counts[step]; ++i) {
      const size_t state = steps[(step * sg_dfa_state_count(dfa)) + i];
      struct mgp_value *value = NULL;
      if (!mg_ok(mgp_value_make_string(sg_dfa_state_key(dfa, state), memory, &value)) ||
          value == NULL || !mg_ok(mgp_list_append_move(active, value))) {
        mgp_value_destroy(value);
        mgp_list_destroy(active);
        set_error(result, "failed to append active state");
        active = NULL;
        break;
      }
    }
    if (active == NULL) {
      break;
    }
    struct mgp_value *active_value = NULL;
    if (!mg_ok(mgp_value_make_list(active, &active_value)) || active_value == NULL ||
        !insert_value(record, "active_state_keys", active_value)) {
      mgp_list_destroy(active);
      set_error(result, "failed to insert active states");
      break;
    }
  }

  sg_explain_free(steps, counts);
  free(states);
  sg_word_free(&word);
  sg_dfa_free(dfa);
}

static void mark_dirty_cb(struct mgp_list *args, struct mgp_graph *graph, struct mgp_result *result,
                          struct mgp_memory *memory) {
  const char *model = NULL;
  if (!get_arg_string(args, 0U, &model)) {
    set_error(result, "mark_dirty requires a string model argument");
    return;
  }
  static const char *query = "MATCH (m:SyncModel {model: $model}) "
                             "SET m.dirty = true, m.generation = coalesce(m.generation, 0) + 1 "
                             "RETURN m.generation AS generation";
  struct mgp_map *params = make_model_params(model, memory);
  struct mgp_execution_result *exec = NULL;
  if (params == NULL || !mg_ok(mgp_execute_query(graph, memory, query, params, &exec)) ||
      exec == NULL) {
    mgp_map_destroy(params);
    set_error(result, "failed to mark model dirty");
    return;
  }
  struct mgp_map *row = NULL;
  int64_t generation = 0;
  if (mg_ok(mgp_pull_one(exec, graph, memory, &row)) && row != NULL) {
    struct mgp_value *generation_value = NULL;
    if (mg_ok(mgp_map_at(row, "generation", &generation_value)) && generation_value != NULL) {
      (void)mgp_value_get_int(generation_value, &generation);
    }
  }
  struct mgp_result_record *record = NULL;
  if (!new_record(result, &record) || !insert_string(record, "status", "DIRTY", memory) ||
      !insert_int(record, "generation", generation, memory)) {
    set_error(result, "failed to insert dirty result");
  }
  mgp_execution_result_destroy(exec);
  mgp_map_destroy(params);
}

static void on_transition_delta_cb(struct mgp_list *args, struct mgp_graph *graph,
                                   struct mgp_result *result, struct mgp_memory *memory) {
  mark_dirty_cb(args, graph, result, memory);
}

static bool add_result(struct mgp_proc *proc, const char *name, struct mgp_type *type) {
  return mg_ok(mgp_proc_add_result(proc, name, type));
}

static bool add_required(struct mgp_proc *proc, const char *name, struct mgp_type *type) {
  return mg_ok(mgp_proc_add_arg(proc, name, type));
}

static bool add_optional_string(struct mgp_proc *proc, const char *name, const char *default_value,
                                struct mgp_memory *memory, struct mgp_type *string_type) {
  struct mgp_value *value = NULL;
  if (!mg_ok(mgp_value_make_string(default_value, memory, &value)) || value == NULL) {
    return false;
  }
  const bool ok = mg_ok(mgp_proc_add_opt_arg(proc, name, string_type, value));
  mgp_value_destroy(value);
  return ok;
}

static bool add_optional_int(struct mgp_proc *proc, const char *name, int64_t default_value,
                             struct mgp_memory *memory, struct mgp_type *int_type) {
  struct mgp_value *value = NULL;
  if (!mg_ok(mgp_value_make_int(default_value, memory, &value)) || value == NULL) {
    return false;
  }
  const bool ok = mg_ok(mgp_proc_add_opt_arg(proc, name, int_type, value));
  mgp_value_destroy(value);
  return ok;
}

static bool add_optional_bool(struct mgp_proc *proc, const char *name, bool default_value,
                              struct mgp_memory *memory, struct mgp_type *bool_type) {
  struct mgp_value *value = NULL;
  if (!mg_ok(mgp_value_make_bool(default_value ? 1 : 0, memory, &value)) || value == NULL) {
    return false;
  }
  const bool ok = mg_ok(mgp_proc_add_opt_arg(proc, name, bool_type, value));
  mgp_value_destroy(value);
  return ok;
}

static bool add_optional_empty_list(struct mgp_proc *proc, const char *name,
                                    struct mgp_memory *memory, struct mgp_type *list_type) {
  struct mgp_list *list = NULL;
  if (!mg_ok(mgp_list_make_empty(0U, memory, &list)) || list == NULL) {
    return false;
  }
  struct mgp_value *value = NULL;
  if (!mg_ok(mgp_value_make_list(list, &value)) || value == NULL) {
    mgp_list_destroy(list);
    return false;
  }
  const bool ok = mg_ok(mgp_proc_add_opt_arg(proc, name, list_type, value));
  mgp_value_destroy(value);
  return ok;
}

static bool register_validate(struct mgp_module *module, struct mgp_type *string_type,
                              struct mgp_type *bool_type, struct mgp_type *int_type) {
  struct mgp_proc *proc = NULL;
  if (!mg_ok(mgp_module_add_read_procedure(module, "validate_model", validate_model_cb, &proc)) ||
      proc == NULL) {
    return false;
  }
  return add_required(proc, "model", string_type) && add_result(proc, "ok", bool_type) &&
         add_result(proc, "code", string_type) && add_result(proc, "message", string_type) &&
         add_result(proc, "states", int_type) && add_result(proc, "letters", int_type) &&
         add_result(proc, "transitions", int_type);
}

static bool register_build_model(struct mgp_module *module, struct mgp_memory *memory,
                                 struct mgp_type *string_type, struct mgp_type *bool_type,
                                 struct mgp_type *int_type) {
  struct mgp_proc *proc = NULL;
  if (!mg_ok(mgp_module_add_read_procedure(module, "build_model", build_model_cb, &proc)) ||
      proc == NULL) {
    return false;
  }
  return add_required(proc, "model", string_type) &&
         add_optional_bool(proc, "complete_with_sink", false, memory, bool_type) &&
         add_result(proc, "status", string_type) && add_result(proc, "generation", int_type) &&
         add_result(proc, "states", int_type) && add_result(proc, "letters", int_type) &&
         add_result(proc, "transitions", int_type);
}

static bool register_build_pair_oracle(struct mgp_module *module, struct mgp_memory *memory,
                                       struct mgp_type *string_type, struct mgp_type *bool_type,
                                       struct mgp_type *int_type) {
  struct mgp_proc *proc = NULL;
  if (!mg_ok(mgp_module_add_write_procedure(module, "build_pair_oracle", build_pair_oracle_cb,
                                            &proc)) ||
      proc == NULL) {
    return false;
  }
  return add_required(proc, "model", string_type) &&
         add_optional_bool(proc, "materialize", true, memory, bool_type) &&
         add_result(proc, "status", string_type) && add_result(proc, "pairs", int_type) &&
         add_result(proc, "pair_edges", int_type) &&
         add_result(proc, "mergeable_pairs", int_type) &&
         add_result(proc, "materialized", bool_type) && add_result(proc, "generation", int_type);
}

static bool register_word_for_set(struct mgp_module *module, struct mgp_memory *memory,
                                  struct mgp_type *string_type, struct mgp_type *int_type,
                                  struct mgp_type *list_type) {
  struct mgp_proc *proc = NULL;
  if (!mg_ok(mgp_module_add_read_procedure(module, "word_for_set", word_for_set_cb, &proc)) ||
      proc == NULL) {
    return false;
  }
  return add_required(proc, "model", string_type) && add_required(proc, "state_keys", list_type) &&
         add_optional_empty_list(proc, "target_keys", memory, list_type) &&
         add_optional_string(proc, "mode", "SYNC", memory, string_type) &&
         add_optional_int(proc, "budget", 0, memory, int_type) &&
         add_result(proc, "status", string_type) && add_result(proc, "word", list_type) &&
         add_result(proc, "length", int_type) && add_result(proc, "final_state_key", string_type) &&
         add_result(proc, "generation", int_type);
}

static bool register_word_to_target(struct mgp_module *module, struct mgp_memory *memory,
                                    struct mgp_type *string_type, struct mgp_type *int_type,
                                    struct mgp_type *list_type) {
  struct mgp_proc *proc = NULL;
  if (!mg_ok(mgp_module_add_read_procedure(module, "word_to_target", word_to_target_cb, &proc)) ||
      proc == NULL) {
    return false;
  }
  return add_required(proc, "model", string_type) && add_required(proc, "state_keys", list_type) &&
         add_required(proc, "target_keys", list_type) &&
         add_optional_string(proc, "mode", "REACH_AND_SYNC", memory, string_type) &&
         add_optional_int(proc, "budget", 0, memory, int_type) &&
         add_result(proc, "status", string_type) && add_result(proc, "word", list_type) &&
         add_result(proc, "length", int_type) && add_result(proc, "final_state_key", string_type) &&
         add_result(proc, "generation", int_type);
}

static bool register_explain(struct mgp_module *module, struct mgp_type *string_type,
                             struct mgp_type *int_type, struct mgp_type *list_type) {
  struct mgp_proc *proc = NULL;
  if (!mg_ok(mgp_module_add_read_procedure(module, "explain", explain_cb, &proc)) || proc == NULL) {
    return false;
  }
  return add_required(proc, "model", string_type) && add_required(proc, "state_keys", list_type) &&
         add_required(proc, "word", list_type) && add_result(proc, "step", int_type) &&
         add_result(proc, "letter", string_type) &&
         add_result(proc, "active_state_keys", list_type);
}

static bool register_expand_cache(struct mgp_module *module, struct mgp_memory *memory,
                                  struct mgp_type *string_type, struct mgp_type *int_type,
                                  struct mgp_type *list_type) {
  struct mgp_proc *proc = NULL;
  if (!mg_ok(mgp_module_add_write_procedure(module, "expand_cache", expand_cache_cb, &proc)) ||
      proc == NULL) {
    return false;
  }
  return add_required(proc, "model", string_type) && add_required(proc, "target_keys", list_type) &&
         add_optional_string(proc, "mode", "REACH_AND_SYNC", memory, string_type) &&
         add_optional_int(proc, "budget", 0, memory, int_type) &&
         add_result(proc, "status", string_type) && add_result(proc, "expanded", int_type) &&
         add_result(proc, "cache_size", int_type);
}

static bool register_dirty_write(struct mgp_module *module, const char *name, mgp_proc_cb callback,
                                 struct mgp_type *string_type, struct mgp_type *int_type) {
  struct mgp_proc *proc = NULL;
  if (!mg_ok(mgp_module_add_write_procedure(module, name, callback, &proc)) || proc == NULL) {
    return false;
  }
  return add_required(proc, "model", string_type) && add_result(proc, "status", string_type) &&
         add_result(proc, "generation", int_type);
}

int mgp_init_module(struct mgp_module *module, struct mgp_memory *memory) {
  struct mgp_type *string_type = NULL;
  struct mgp_type *bool_type = NULL;
  struct mgp_type *int_type = NULL;
  struct mgp_type *any_type = NULL;
  struct mgp_type *nullable_any_type = NULL;
  struct mgp_type *list_type = NULL;
  if (!mg_ok(mgp_type_string(&string_type)) || !mg_ok(mgp_type_bool(&bool_type)) ||
      !mg_ok(mgp_type_int(&int_type)) || !mg_ok(mgp_type_any(&any_type)) ||
      !mg_ok(mgp_type_nullable(any_type, &nullable_any_type)) ||
      !mg_ok(mgp_type_list(nullable_any_type, &list_type))) {
    return 1;
  }

  if (!register_validate(module, string_type, bool_type, int_type) ||
      !register_build_model(module, memory, string_type, bool_type, int_type) ||
      !register_build_pair_oracle(module, memory, string_type, bool_type, int_type) ||
      !register_word_for_set(module, memory, string_type, int_type, list_type) ||
      !register_word_to_target(module, memory, string_type, int_type, list_type) ||
      !register_expand_cache(module, memory, string_type, int_type, list_type) ||
      !register_explain(module, string_type, int_type, list_type) ||
      !register_dirty_write(module, "mark_dirty", mark_dirty_cb, string_type, int_type) ||
      !register_dirty_write(module, "on_transition_delta", on_transition_delta_cb, string_type,
                            int_type)) {
    return 1;
  }
  return 0;
}

int mgp_shutdown_module(void) {
  return 0;
}
