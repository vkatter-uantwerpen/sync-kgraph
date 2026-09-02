#include "sync_kgraph/sync.h"

#include "dynamic.h"
#include "snapshot.h"
#include "snapshot_cache.h"

#include "mg_procedure.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SG_MATERIALIZE_BATCH 512U
#define SG_HYDRATE_BATCH 4096U
#define SG_ERROR_MESSAGE_CAPACITY 192U
#define SG_SNAPSHOT_INCARNATION_CAPACITY 33U
#define SG_SNAPSHOT_TOKEN_CAPACITY 64U
#define SG_SNAPSHOT_CACHE_DEFAULT_BYTES ((size_t)512U * (size_t)1024U * (size_t)1024U)
#define SG_VALIDATE_REPORTED_ARGUMENT 5U
#define SG_VALIDATE_AVAILABLE_ARGUMENT 6U

// NOLINTNEXTLINE(misc-use-internal-linkage)
int mgp_init_module(struct mgp_module *module, struct mgp_memory *memory);
// NOLINTNEXTLINE(misc-use-internal-linkage)
int mgp_shutdown_module(void);

typedef struct {
  uint64_t generation;
  uint64_t oracle_epoch;
  bool dirty;
  bool prepared;
  bool incremental;
  bool pair_edges_materialized;
  char snapshot_token[SG_SNAPSHOT_TOKEN_CAPACITY];
} model_metadata;

typedef enum {
  DOMAIN_STATE = 0,
  DOMAIN_ACTION,
  DOMAIN_OUTPUT,
} domain_kind;

typedef enum {
  ORACLE_SOURCE_PERSISTED = 0,
  ORACLE_SOURCE_RECOMPUTED,
} oracle_source;

typedef enum {
  CACHE_STATE_HOT = 0,
  CACHE_STATE_HYDRATED,
  CACHE_STATE_BYPASSED,
} cache_state;

typedef struct {
  oracle_source source;
  size_t oracle_builds;
  size_t oracle_rows_loaded;
  size_t oracle_load_batches;
  size_t oracle_cache_hits;
  size_t snapshot_record_reads;
  uint64_t oracle_time_us;
  uint64_t snapshot_hydration_time_us;
  uint64_t total_compute_time_us;
  cache_state cache;
} planning_metrics;

typedef struct {
  const sg_pair_snapshot *snapshot;
  planning_metrics *metrics;
} snapshot_record_context;

typedef struct {
  struct mgp_graph *graph;
  struct mgp_memory *memory;
  const char *model;
  uint64_t oracle_epoch;
  uint64_t updated_generation;
  const sg_automaton *automaton;
} memgraph_pair_store;

typedef struct {
  const char *state_key;
  const char *action_key;
  const char *target_key;
  const char *output_key;
  size_t state;
  size_t action;
  size_t target;
  size_t output;
} cell_change;

typedef struct {
  size_t pair;
  size_t action;
  size_t next_pair;
  bool outputs_differ;
} pair_edge_update;

typedef struct {
  struct mgp_result *result;
  const sg_automaton *automaton;
  struct mgp_memory *memory;
} explain_context;

static sg_snapshot_cache *snapshot_cache = NULL;
static char snapshot_incarnation[SG_SNAPSHOT_INCARNATION_CAPACITY] = {0};
static atomic_uint_fast64_t snapshot_counter = UINT64_C(1);

static bool mg_ok(enum mgp_error error) {
  return error == MGP_ERROR_NO_ERROR;
}

static uint64_t monotonic_time_us(void) {
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

static uint64_t elapsed_us(uint64_t start) {
  const uint64_t end = monotonic_time_us();
  return end >= start ? end - start : 0U;
}

static const char *oracle_source_name(oracle_source source) {
  switch (source) {
  case ORACLE_SOURCE_PERSISTED:
    return "PERSISTED";
  case ORACLE_SOURCE_RECOMPUTED:
    return "RECOMPUTED";
  }
  return "UNKNOWN";
}

static const char *cache_state_name(cache_state state) {
  switch (state) {
  case CACHE_STATE_HOT:
    return "HOT";
  case CACHE_STATE_HYDRATED:
    return "HYDRATED";
  case CACHE_STATE_BYPASSED:
    return "BYPASSED";
  }
  return "UNKNOWN";
}

static bool initialize_snapshot_incarnation(void) {
  unsigned char bytes[16] = {0};
  FILE *random = fopen("/dev/urandom", "rb");
  if (random == NULL) {
    return false;
  }
  const bool read = fread(bytes, sizeof(bytes), 1U, random) == 1U;
  const bool closed = fclose(random) == 0;
  if (!read || !closed) {
    return false;
  }
  for (size_t index = 0U; index < sizeof(bytes); ++index) {
    const int written = snprintf(&snapshot_incarnation[index * 2U], 3U, "%02x", bytes[index]);
    if (written != 2) {
      return false;
    }
  }
  snapshot_incarnation[sizeof(snapshot_incarnation) - 1U] = '\0';
  return true;
}

static bool make_snapshot_token(char token[SG_SNAPSHOT_TOKEN_CAPACITY]) {
  const uint_fast64_t counter =
      atomic_fetch_add_explicit(&snapshot_counter, UINT64_C(1), memory_order_relaxed);
  if (counter == UINT_FAST64_MAX) {
    return false;
  }
  const int written = snprintf(token, SG_SNAPSHOT_TOKEN_CAPACITY, "%s-%016" PRIxFAST64,
                               snapshot_incarnation, counter);
  return written > 0 && (size_t)written < SG_SNAPSHOT_TOKEN_CAPACITY;
}

static bool snapshot_cache_limit(size_t *limit) {
  const char *configured = getenv("SYNC_KGRAPH_CACHE_MAX_BYTES");
  if (configured == NULL || configured[0] == '\0') {
    *limit = SG_SNAPSHOT_CACHE_DEFAULT_BYTES;
    return true;
  }
  if (configured[0] == '-') {
    return false;
  }
  errno = 0;
  char *end = NULL;
  const unsigned long long parsed = strtoull(configured, &end, 10);
  if (errno != 0 || end == configured || *end != '\0' || parsed > (unsigned long long)SIZE_MAX) {
    return false;
  }
  *limit = (size_t)parsed;
  return true;
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

static bool get_int_arg_default(struct mgp_list *arguments, size_t index, int64_t default_value,
                                int64_t *value) {
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
  return type == MGP_VALUE_TYPE_INT && mg_ok(mgp_value_get_int(argument, value));
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

static bool row_pair_record(struct mgp_map *row, sg_pair_record *record) {
  int64_t pair = -1;
  int64_t merge_distance = -1;
  int64_t merge_action = -1;
  int64_t merge_next = -1;
  int64_t merge_support_count = -1;
  int64_t resolution_distance = -1;
  int64_t resolution_action = -1;
  int64_t resolution_next = -1;
  int64_t resolution_support_count = -1;
  bool mergeable = false;
  bool resolvable = false;
  if (!row_int(row, "pair_id", &pair) || !row_bool(row, "mergeable", &mergeable) ||
      !row_int(row, "merge_distance", &merge_distance) ||
      !row_int(row, "merge_action_id", &merge_action) ||
      !row_int(row, "merge_next_pair", &merge_next) ||
      !row_int(row, "merge_support_count", &merge_support_count) ||
      !row_bool(row, "resolvable", &resolvable) ||
      !row_int(row, "resolution_distance", &resolution_distance) ||
      !row_int(row, "resolution_action_id", &resolution_action) ||
      !row_int(row, "resolution_next_pair", &resolution_next) ||
      !row_int(row, "resolution_support_count", &resolution_support_count) || pair < 0 ||
      merge_support_count < 0 || resolution_support_count < 0 ||
      !int64_to_index(merge_distance, &record->merge_distance) ||
      !int64_to_index(merge_action, &record->merge_action) ||
      !int64_to_index(merge_next, &record->merge_next_pair) ||
      !int64_to_index(resolution_distance, &record->resolution_distance) ||
      !int64_to_index(resolution_action, &record->resolution_action) ||
      !int64_to_index(resolution_next, &record->resolution_next_pair)) {
    return false;
  }
  record->pair = (size_t)pair;
  record->mergeable = mergeable;
  record->merge_support_count = (size_t)merge_support_count;
  record->resolvable = resolvable;
  record->resolution_support_count = (size_t)resolution_support_count;
  return true;
}

static sg_status load_metadata(struct mgp_graph *graph, struct mgp_memory *memory,
                               const char *model, model_metadata *metadata) {
  static const char *query = "MATCH (m:SyncModel {model: $model}) "
                             "RETURN coalesce(m.generation, 0) AS generation, "
                             "coalesce(m.oracle_epoch, 0) AS oracle_epoch, "
                             "coalesce(m.dirty, true) AS dirty, "
                             "coalesce(m.prepared_generation, -1) AS prepared_generation, "
                             "coalesce(m.incremental, false) AS incremental, "
                             "coalesce(m.pair_edges_materialized, false) "
                             "AS pair_edges_materialized, "
                             "coalesce(m.snapshot_token, \"\") AS snapshot_token";
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
  int64_t oracle_epoch = -1;
  int64_t prepared_generation = -1;
  bool dirty = true;
  bool incremental = false;
  bool pair_edges_materialized = false;
  const char *snapshot_token = NULL;
  if (status == SG_OK &&
      (!mg_ok(mgp_pull_one(execution, graph, memory, &row)) || row == NULL ||
       !row_int(row, "generation", &generation) || !row_int(row, "oracle_epoch", &oracle_epoch) ||
       !row_bool(row, "dirty", &dirty) ||
       !row_int(row, "prepared_generation", &prepared_generation) || generation < 0 ||
       oracle_epoch < 0)) {
    status = SG_ERR_NOT_FOUND;
  }
  if (status == SG_OK && !row_bool(row, "incremental", &incremental)) {
    status = SG_ERR_INVALID_MODEL;
  }
  if (status == SG_OK && (!row_bool(row, "pair_edges_materialized", &pair_edges_materialized) ||
                          !row_string(row, "snapshot_token", &snapshot_token) ||
                          strlen(snapshot_token) >= SG_SNAPSHOT_TOKEN_CAPACITY)) {
    status = SG_ERR_INVALID_MODEL;
  }
  struct mgp_map *extra = NULL;
  if (status == SG_OK &&
      (!mg_ok(mgp_pull_one(execution, graph, memory, &extra)) || extra != NULL)) {
    status = SG_ERR_INVALID_MODEL;
  }
  if (status == SG_OK) {
    metadata->generation = (uint64_t)generation;
    metadata->oracle_epoch = (uint64_t)oracle_epoch;
    metadata->dirty = dirty;
    metadata->prepared = !dirty && oracle_epoch > 0 && prepared_generation == generation &&
                         snapshot_token[0] != '\0';
    metadata->incremental = incremental;
    metadata->pair_edges_materialized = pair_edges_materialized;
    (void)memcpy(metadata->snapshot_token, snapshot_token, strlen(snapshot_token) + 1U);
  }
  if (execution != NULL) {
    mgp_execution_result_destroy(execution);
  }
  mgp_map_destroy(params);
  return status;
}

static sg_status load_generation(struct mgp_graph *graph, struct mgp_memory *memory,
                                 const char *model, uint64_t *generation) {
  static const char *query = "MATCH (m:SyncModel {model: $model}) "
                             "RETURN coalesce(m.generation, 0) AS generation";
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
  int64_t converted = -1;
  if (status == SG_OK && (!mg_ok(mgp_pull_one(execution, graph, memory, &row)) || row == NULL ||
                          !row_int(row, "generation", &converted) || converted < 0)) {
    status = SG_ERR_NOT_FOUND;
  }
  struct mgp_map *extra = NULL;
  if (status == SG_OK &&
      (!mg_ok(mgp_pull_one(execution, graph, memory, &extra)) || extra != NULL)) {
    status = SG_ERR_INVALID_MODEL;
  }
  if (status == SG_OK) {
    *generation = (uint64_t)converted;
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

static sg_status load_automaton_generation(struct mgp_graph *graph, struct mgp_memory *memory,
                                           const char *model, uint64_t generation,
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

  sg_automaton_builder *builder = NULL;
  sg_status status = sg_automaton_builder_init(&builder);
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
    status = sg_automaton_builder_build(builder, generation, automaton);
  }
  sg_automaton_builder_free(builder);
  return status;
}

static sg_status load_automaton(struct mgp_graph *graph, struct mgp_memory *memory,
                                const char *model, model_metadata *metadata,
                                sg_automaton **automaton) {
  const sg_status status = load_metadata(graph, memory, model, metadata);
  if (status != SG_OK) {
    return status;
  }
  return load_automaton_generation(graph, memory, model, metadata->generation, automaton);
}

static sg_status load_base_automaton(struct mgp_graph *graph, struct mgp_memory *memory,
                                     const char *model, sg_automaton **automaton) {
  uint64_t generation = 0U;
  const sg_status status = load_generation(graph, memory, model, &generation);
  if (status != SG_OK) {
    return status;
  }
  return load_automaton_generation(graph, memory, model, generation, automaton);
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

static size_t pair_index_for_states(size_t state_count, size_t first, size_t second) {
  if (first > second) {
    const size_t temporary = first;
    first = second;
    second = temporary;
  }
  return (first * state_count) - ((first * (first + 1U)) / 2U) + second;
}

static bool append_store_record(struct mgp_list *list, const memgraph_pair_store *store,
                                const sg_pair_record *record) {
  const char *merge_action = record->merge_action == SG_INDEX_NONE
                                 ? ""
                                 : sg_automaton_action_key(store->automaton, record->merge_action);
  const char *resolution_action =
      record->resolution_action == SG_INDEX_NONE
          ? ""
          : sg_automaton_action_key(store->automaton, record->resolution_action);
  int64_t pair = 0;
  int64_t generation = 0;
  int64_t merge_support_count = 0;
  int64_t resolution_support_count = 0;
  if (merge_action == NULL || resolution_action == NULL || !size_to_int64(record->pair, &pair) ||
      !uint64_to_int64(store->updated_generation, &generation) ||
      !size_to_int64(record->merge_support_count, &merge_support_count) ||
      !size_to_int64(record->resolution_support_count, &resolution_support_count)) {
    return false;
  }
  struct mgp_map *map = NULL;
  if (!mg_ok(mgp_unordered_map_make_empty(store->memory, &map)) || map == NULL) {
    return false;
  }
  const bool populated =
      map_insert_int(map, "pair_id", pair, store->memory) &&
      map_insert_int(map, "updated_generation", generation, store->memory) &&
      map_insert_bool(map, "mergeable", record->mergeable, store->memory) &&
      map_insert_int(map, "merge_distance", index_to_int64(record->merge_distance),
                     store->memory) &&
      map_insert_string(map, "merge_action", merge_action, store->memory) &&
      map_insert_int(map, "merge_action_id", index_to_int64(record->merge_action), store->memory) &&
      map_insert_int(map, "merge_next_pair", index_to_int64(record->merge_next_pair),
                     store->memory) &&
      map_insert_int(map, "merge_support_count", merge_support_count, store->memory) &&
      map_insert_bool(map, "resolvable", record->resolvable, store->memory) &&
      map_insert_int(map, "resolution_distance", index_to_int64(record->resolution_distance),
                     store->memory) &&
      map_insert_string(map, "resolution_action", resolution_action, store->memory) &&
      map_insert_int(map, "resolution_action_id", index_to_int64(record->resolution_action),
                     store->memory) &&
      map_insert_int(map, "resolution_next_pair", index_to_int64(record->resolution_next_pair),
                     store->memory) &&
      map_insert_int(map, "resolution_support_count", resolution_support_count, store->memory);
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

static sg_status memgraph_store_write_record_batch(memgraph_pair_store *store,
                                                   const sg_pair_record *records,
                                                   size_t record_count) {
  static const char *query =
      "UNWIND $records AS r "
      "MATCH (p:SyncPair {model: $model, oracle_epoch: $oracle_epoch, pair_id: r.pair_id}) "
      "SET p.updated_generation = r.updated_generation, p.mergeable = r.mergeable, "
      "p.merge_distance = r.merge_distance, p.merge_action = r.merge_action, "
      "p.merge_action_id = r.merge_action_id, p.merge_next_pair = r.merge_next_pair, "
      "p.merge_support_count = r.merge_support_count, p.resolvable = r.resolvable, "
      "p.resolution_distance = r.resolution_distance, "
      "p.resolution_action = r.resolution_action, "
      "p.resolution_action_id = r.resolution_action_id, "
      "p.resolution_next_pair = r.resolution_next_pair, "
      "p.resolution_support_count = r.resolution_support_count";
  struct mgp_list *list = NULL;
  if (!mg_ok(mgp_list_make_empty(record_count, store->memory, &list)) || list == NULL) {
    return SG_ERR_ALLOC;
  }
  for (size_t index = 0U; index < record_count; ++index) {
    if (!append_store_record(list, store, &records[index])) {
      mgp_list_destroy(list);
      return SG_ERR_ALLOC;
    }
  }
  struct mgp_value *records_value = NULL;
  if (!mg_ok(mgp_value_make_list(list, &records_value)) || records_value == NULL) {
    mgp_list_destroy(list);
    return SG_ERR_ALLOC;
  }
  struct mgp_map *params = make_model_params(store->model, store->memory);
  int64_t epoch = 0;
  if (params == NULL) {
    mgp_value_destroy(records_value);
    return SG_ERR_ALLOC;
  }
  if (!uint64_to_int64(store->oracle_epoch, &epoch) ||
      !params_insert_int(params, "oracle_epoch", epoch, store->memory)) {
    mgp_value_destroy(records_value);
    mgp_map_destroy(params);
    return SG_ERR_ALLOC;
  }
  if (!params_insert_value(params, "records", records_value)) {
    mgp_map_destroy(params);
    return SG_ERR_ALLOC;
  }
  const bool ok = execute_drain(store->graph, store->memory, query, params);
  mgp_map_destroy(params);
  return ok ? SG_OK : SG_ERR_INVALID_MODEL;
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

static bool map_optional_string(struct mgp_map *map, const char *key, const char **value,
                                bool *present) {
  struct mgp_value *entry = NULL;
  if (!mg_ok(mgp_map_at(map, key, &entry)) || entry == NULL) {
    *value = NULL;
    *present = false;
    return true;
  }
  *present = true;
  return mg_ok(mgp_value_get_string(entry, value)) && *value != NULL;
}

static bool parse_cell_changes(const sg_automaton *automaton, struct mgp_list *arguments,
                               cell_change **changes, size_t *change_count) {
  *changes = NULL;
  *change_count = 0U;
  struct mgp_value *argument = NULL;
  struct mgp_list *list = NULL;
  size_t count = 0U;
  if (!get_arg(arguments, 1U, &argument) || !mg_ok(mgp_value_get_list(argument, &list)) ||
      list == NULL || !mg_ok(mgp_list_size(list, &count)) || count == 0U) {
    return false;
  }
  cell_change *parsed = calloc(count, sizeof(*parsed));
  const size_t state_count = sg_automaton_state_count(automaton);
  const size_t action_count = sg_automaton_action_count(automaton);
  if (state_count == 0U || action_count == 0U || action_count > SIZE_MAX / state_count) {
    free(parsed);
    return false;
  }
  bool *seen = calloc(state_count * action_count, sizeof(*seen));
  if (parsed == NULL || seen == NULL) {
    free(seen);
    free(parsed);
    return false;
  }
  size_t kept = 0U;
  bool ok = true;
  for (size_t index = 0U; ok && index < count; ++index) {
    struct mgp_value *item = NULL;
    struct mgp_map *map = NULL;
    const char *state_key = NULL;
    const char *action_key = NULL;
    const char *target_key = NULL;
    const char *output_key = NULL;
    bool state_present = false;
    bool action_present = false;
    bool target_present = false;
    bool output_present = false;
    if (!mg_ok(mgp_list_at(list, index, &item)) || item == NULL ||
        !mg_ok(mgp_value_get_map(item, &map)) || map == NULL ||
        !map_optional_string(map, "state_key", &state_key, &state_present) || !state_present ||
        !map_optional_string(map, "action_key", &action_key, &action_present) || !action_present ||
        !map_optional_string(map, "target_key", &target_key, &target_present) ||
        !map_optional_string(map, "output_key", &output_key, &output_present) ||
        (!target_present && !output_present)) {
      ok = false;
      break;
    }
    size_t state = 0U;
    size_t action = 0U;
    size_t target = 0U;
    size_t output = 0U;
    if (sg_automaton_find_state(automaton, state_key, &state) != SG_OK ||
        sg_automaton_find_action(automaton, action_key, &action) != SG_OK) {
      ok = false;
      break;
    }
    target = target_present ? 0U : sg_automaton_transition(automaton, state, action);
    output = output_present ? 0U : sg_automaton_observation(automaton, state, action);
    if ((target_present && sg_automaton_find_state(automaton, target_key, &target) != SG_OK) ||
        (output_present && sg_automaton_find_output(automaton, output_key, &output) != SG_OK)) {
      ok = false;
      break;
    }
    const size_t cell = (state * action_count) + action;
    if (seen[cell]) {
      ok = false;
      break;
    }
    seen[cell] = true;
    if (target == sg_automaton_transition(automaton, state, action) &&
        output == sg_automaton_observation(automaton, state, action)) {
      continue;
    }
    parsed[kept] = (cell_change){
        .state_key = state_key,
        .action_key = action_key,
        .target_key = sg_automaton_state_key(automaton, target),
        .output_key = sg_automaton_output_key(automaton, output),
        .state = state,
        .action = action,
        .target = target,
        .output = output,
    };
    if (parsed[kept].target_key == NULL || parsed[kept].output_key == NULL) {
      ok = false;
      break;
    }
    ++kept;
  }
  if (!ok) {
    free(seen);
    free(parsed);
    return false;
  }
  free(seen);
  *changes = parsed;
  *change_count = kept;
  return true;
}

static bool append_cell_change(struct mgp_list *list, const cell_change *change,
                               struct mgp_memory *memory) {
  struct mgp_map *map = NULL;
  if (!mg_ok(mgp_unordered_map_make_empty(memory, &map)) || map == NULL) {
    return false;
  }
  const bool populated = map_insert_string(map, "state_key", change->state_key, memory) &&
                         map_insert_string(map, "action_key", change->action_key, memory) &&
                         map_insert_string(map, "target_key", change->target_key, memory) &&
                         map_insert_string(map, "output_key", change->output_key, memory);
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

static bool apply_base_changes(struct mgp_graph *graph, struct mgp_memory *memory,
                               const char *model, const cell_change *changes, size_t change_count) {
  static const char *query =
      "UNWIND $changes AS c "
      "MATCH (src:SyncState {model: $model, state_key: c.state_key}), "
      "(dst:SyncState {model: $model, state_key: c.target_key}), "
      "(out:SyncOutput {model: $model, output_key: c.output_key}) "
      "MATCH (src)-[old_t:SYNC_TRANS {model: $model, action_key: c.action_key}]->() "
      "MATCH (src)-[old_o:SYNC_OBS {model: $model, action_key: c.action_key}]->() "
      "DELETE old_t, old_o "
      "CREATE (src)-[:SYNC_TRANS {model: $model, action_key: c.action_key}]->(dst) "
      "CREATE (src)-[:SYNC_OBS {model: $model, action_key: c.action_key}]->(out)";
  struct mgp_list *list = NULL;
  if (!mg_ok(mgp_list_make_empty(change_count, memory, &list)) || list == NULL) {
    return false;
  }
  for (size_t index = 0U; index < change_count; ++index) {
    if (!append_cell_change(list, &changes[index], memory)) {
      mgp_list_destroy(list);
      return false;
    }
  }
  struct mgp_value *changes_value = NULL;
  if (!mg_ok(mgp_value_make_list(list, &changes_value)) || changes_value == NULL) {
    mgp_list_destroy(list);
    return false;
  }
  struct mgp_map *params = make_model_params(model, memory);
  if (!params_insert_value(params, "changes", changes_value)) {
    if (params != NULL) {
      mgp_map_destroy(params);
    }
    return false;
  }
  const bool ok = execute_drain(graph, memory, query, params);
  mgp_map_destroy(params);
  return ok;
}

static int compare_edge_updates(const void *left, const void *right) {
  const pair_edge_update *first = left;
  const pair_edge_update *second = right;
  if (first->pair != second->pair) {
    return first->pair < second->pair ? -1 : 1;
  }
  if (first->action != second->action) {
    return first->action < second->action ? -1 : 1;
  }
  return 0;
}

static int compare_size_values(const void *left, const void *right) {
  const size_t first = *(const size_t *)left;
  const size_t second = *(const size_t *)right;
  if (first == second) {
    return 0;
  }
  return first < second ? -1 : 1;
}

static sg_status build_edge_updates(const sg_automaton *automaton, const cell_change *changes,
                                    size_t change_count, pair_edge_update **updates,
                                    size_t *update_count, size_t **seed_pairs, size_t *seed_count) {
  *updates = NULL;
  *update_count = 0U;
  *seed_pairs = NULL;
  *seed_count = 0U;
  const size_t state_count = sg_automaton_state_count(automaton);
  const size_t action_count = sg_automaton_action_count(automaton);
  if (state_count == 0U || action_count == 0U) {
    return SG_ERR_INVALID_MODEL;
  }
  if (change_count > SIZE_MAX / state_count) {
    return SG_ERR_ALLOC;
  }
  const size_t maximum_updates = change_count * state_count;
  pair_edge_update *created_updates = calloc(maximum_updates, sizeof(*created_updates));
  size_t *created_seeds = calloc(maximum_updates, sizeof(*created_seeds));
  if (created_updates == NULL || created_seeds == NULL) {
    free(created_updates);
    free(created_seeds);
    return SG_ERR_ALLOC;
  }
  size_t created_count = 0U;
  for (size_t change = 0U; change < change_count; ++change) {
    for (size_t other = 0U; other < state_count; ++other) {
      const size_t pair = pair_index_for_states(state_count, changes[change].state, other);
      const size_t first = changes[change].state < other ? changes[change].state : other;
      const size_t second = changes[change].state < other ? other : changes[change].state;
      const size_t action = changes[change].action;
      const size_t first_next = sg_automaton_transition(automaton, first, action);
      const size_t second_next = sg_automaton_transition(automaton, second, action);
      created_updates[created_count] = (pair_edge_update){
          .pair = pair,
          .action = action,
          .next_pair = pair_index_for_states(state_count, first_next, second_next),
          .outputs_differ = sg_automaton_observation(automaton, first, action) !=
                            sg_automaton_observation(automaton, second, action),
      };
      created_seeds[created_count] = pair;
      ++created_count;
    }
  }
  qsort(created_updates, created_count, sizeof(*created_updates), compare_edge_updates);
  qsort(created_seeds, created_count, sizeof(*created_seeds), compare_size_values);
  for (size_t index = 0U; index < created_count; ++index) {
    if (*update_count == 0U ||
        compare_edge_updates(&created_updates[*update_count - 1U], &created_updates[index]) != 0) {
      created_updates[*update_count] = created_updates[index];
      ++*update_count;
    }
    if (*seed_count == 0U || created_seeds[*seed_count - 1U] != created_seeds[index]) {
      created_seeds[*seed_count] = created_seeds[index];
      ++*seed_count;
    }
  }
  *updates = created_updates;
  *seed_pairs = created_seeds;
  return SG_OK;
}

static bool append_edge_update(struct mgp_list *list, const memgraph_pair_store *store,
                               const pair_edge_update *update) {
  const char *action_key = sg_automaton_action_key(store->automaton, update->action);
  int64_t pair = 0;
  int64_t action = 0;
  int64_t next_pair = 0;
  int64_t generation = 0;
  if (action_key == NULL || !size_to_int64(update->pair, &pair) ||
      !size_to_int64(update->action, &action) || !size_to_int64(update->next_pair, &next_pair) ||
      !uint64_to_int64(store->updated_generation, &generation)) {
    return false;
  }
  struct mgp_map *map = NULL;
  if (!mg_ok(mgp_unordered_map_make_empty(store->memory, &map)) || map == NULL) {
    return false;
  }
  const bool populated =
      map_insert_int(map, "pair_id", pair, store->memory) &&
      map_insert_int(map, "action_id", action, store->memory) &&
      map_insert_string(map, "action", action_key, store->memory) &&
      map_insert_int(map, "next_pair", next_pair, store->memory) &&
      map_insert_bool(map, "outputs_differ", update->outputs_differ, store->memory) &&
      map_insert_int(map, "updated_generation", generation, store->memory);
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

static sg_status replace_edge_batch(memgraph_pair_store *store, const pair_edge_update *updates,
                                    size_t update_count) {
  static const char *query =
      "UNWIND $edges AS e "
      "MATCH (p:SyncPair {model: $model, oracle_epoch: $oracle_epoch, pair_id: e.pair_id}) "
      "OPTIONAL MATCH (p)-[old_next:PAIR_NEXT {model: $model, oracle_epoch: $oracle_epoch, "
      "action_id: e.action_id}]->() "
      "OPTIONAL MATCH ()-[old_pre:PAIR_PRE {model: $model, oracle_epoch: $oracle_epoch, "
      "action_id: e.action_id}]->(p) "
      "DELETE old_next, old_pre "
      "WITH p, e "
      "MATCH (n:SyncPair {model: $model, oracle_epoch: $oracle_epoch, "
      "pair_id: e.next_pair}) "
      "CREATE (p)-[:PAIR_NEXT {model: $model, oracle_epoch: $oracle_epoch, "
      "updated_generation: e.updated_generation, action: e.action, "
      "action_id: e.action_id, outputs_differ: e.outputs_differ}]->(n) "
      "CREATE (n)-[:PAIR_PRE {model: $model, oracle_epoch: $oracle_epoch, "
      "updated_generation: e.updated_generation, action: e.action, "
      "action_id: e.action_id, outputs_differ: e.outputs_differ}]->(p)";
  struct mgp_list *list = NULL;
  if (!mg_ok(mgp_list_make_empty(update_count, store->memory, &list)) || list == NULL) {
    return SG_ERR_ALLOC;
  }
  for (size_t index = 0U; index < update_count; ++index) {
    if (!append_edge_update(list, store, &updates[index])) {
      mgp_list_destroy(list);
      return SG_ERR_ALLOC;
    }
  }
  struct mgp_value *edges = NULL;
  if (!mg_ok(mgp_value_make_list(list, &edges)) || edges == NULL) {
    mgp_list_destroy(list);
    return SG_ERR_ALLOC;
  }
  struct mgp_map *params = make_model_params(store->model, store->memory);
  int64_t epoch = 0;
  if (params == NULL) {
    mgp_value_destroy(edges);
    return SG_ERR_ALLOC;
  }
  if (!uint64_to_int64(store->oracle_epoch, &epoch) ||
      !params_insert_int(params, "oracle_epoch", epoch, store->memory)) {
    mgp_value_destroy(edges);
    mgp_map_destroy(params);
    return SG_ERR_ALLOC;
  }
  if (!params_insert_value(params, "edges", edges)) {
    mgp_map_destroy(params);
    return SG_ERR_ALLOC;
  }
  const bool ok = execute_drain(store->graph, store->memory, query, params);
  mgp_map_destroy(params);
  return ok ? SG_OK : SG_ERR_INVALID_MODEL;
}

static sg_status replace_pair_edges(memgraph_pair_store *store, const pair_edge_update *updates,
                                    size_t update_count) {
  for (size_t first = 0U; first < update_count; first += SG_MATERIALIZE_BATCH) {
    const size_t remaining = update_count - first;
    const size_t count = remaining < SG_MATERIALIZE_BATCH ? remaining : SG_MATERIALIZE_BATCH;
    const sg_status status = replace_edge_batch(store, &updates[first], count);
    if (status != SG_OK) {
      return status;
    }
  }
  return SG_OK;
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
                               const sg_pair_oracle *oracle, uint64_t oracle_epoch, size_t pair,
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
  int64_t epoch = 0;
  int64_t generation = 0;
  int64_t first_id = 0;
  int64_t second_id = 0;
  if (merge_action == NULL || resolution_action == NULL || !size_to_int64(pair, &pair_id) ||
      !uint64_to_int64(oracle_epoch, &epoch) ||
      !uint64_to_int64(sg_automaton_generation(automaton), &generation) ||
      !size_to_int64(first, &first_id) || !size_to_int64(second, &second_id)) {
    return false;
  }
  struct mgp_map *map = NULL;
  if (!mg_ok(mgp_unordered_map_make_empty(memory, &map)) || map == NULL) {
    return false;
  }
  const bool populated =
      map_insert_int(map, "pair_id", pair_id, memory) &&
      map_insert_int(map, "oracle_epoch", epoch, memory) &&
      map_insert_int(map, "created_generation", generation, memory) &&
      map_insert_int(map, "updated_generation", generation, memory) &&
      map_insert_string(map, "first_key", sg_automaton_state_key(automaton, first), memory) &&
      map_insert_int(map, "first_id", first_id, memory) &&
      map_insert_string(map, "second_key", sg_automaton_state_key(automaton, second), memory) &&
      map_insert_int(map, "second_id", second_id, memory) &&
      map_insert_bool(map, "mergeable", record.mergeable, memory) &&
      map_insert_int(map, "merge_distance", index_to_int64(record.merge_distance), memory) &&
      map_insert_string(map, "merge_action", merge_action, memory) &&
      map_insert_int(map, "merge_action_id", index_to_int64(record.merge_action), memory) &&
      map_insert_int(map, "merge_next_pair", index_to_int64(record.merge_next_pair), memory) &&
      map_insert_int(map, "merge_support_count", (int64_t)record.merge_support_count, memory) &&
      map_insert_bool(map, "resolvable", record.resolvable, memory) &&
      map_insert_int(map, "resolution_distance", index_to_int64(record.resolution_distance),
                     memory) &&
      map_insert_string(map, "resolution_action", resolution_action, memory) &&
      map_insert_int(map, "resolution_action_id", index_to_int64(record.resolution_action),
                     memory) &&
      map_insert_int(map, "resolution_next_pair", index_to_int64(record.resolution_next_pair),
                     memory) &&
      map_insert_int(map, "resolution_support_count", (int64_t)record.resolution_support_count,
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
                               const sg_pair_oracle *oracle, uint64_t oracle_epoch,
                               size_t first_pair, size_t pair_count) {
  static const char *query =
      "UNWIND $records AS r "
      "CREATE (:SyncPair {model: $model, oracle_epoch: r.oracle_epoch, "
      "created_generation: r.created_generation, updated_generation: r.updated_generation, "
      "pair_id: r.pair_id, "
      "first_key: r.first_key, first_id: r.first_id, second_key: r.second_key, "
      "second_id: r.second_id, mergeable: r.mergeable, merge_distance: r.merge_distance, "
      "merge_action: r.merge_action, merge_action_id: r.merge_action_id, "
      "merge_next_pair: r.merge_next_pair, merge_support_count: r.merge_support_count, "
      "resolvable: r.resolvable, "
      "resolution_distance: r.resolution_distance, resolution_action: r.resolution_action, "
      "resolution_action_id: r.resolution_action_id, "
      "resolution_next_pair: r.resolution_next_pair, "
      "resolution_support_count: r.resolution_support_count})";
  struct mgp_list *records = NULL;
  if (!mg_ok(mgp_list_make_empty(pair_count, memory, &records)) || records == NULL) {
    return false;
  }
  bool ok = true;
  for (size_t offset = 0U; ok && offset < pair_count; ++offset) {
    ok = append_pair_record(records, automaton, oracle, oracle_epoch, first_pair + offset, memory);
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
  if (!params_insert_value(params, "records", records_value)) {
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
                             const sg_pair_oracle *oracle, uint64_t oracle_epoch, size_t edge,
                             struct mgp_memory *memory) {
  const size_t action_count = sg_automaton_action_count(automaton);
  const size_t pair = edge / action_count;
  const size_t action = edge % action_count;
  size_t next_pair = 0U;
  bool outputs_differ = false;
  if (sg_pair_oracle_pair_step(oracle, pair, action, &next_pair, &outputs_differ) != SG_OK) {
    return false;
  }
  int64_t pair_id = 0;
  int64_t epoch = 0;
  int64_t generation = 0;
  int64_t action_id = 0;
  int64_t next_pair_id = 0;
  if (!size_to_int64(pair, &pair_id) || !uint64_to_int64(oracle_epoch, &epoch) ||
      !uint64_to_int64(sg_automaton_generation(automaton), &generation) ||
      !size_to_int64(action, &action_id) || !size_to_int64(next_pair, &next_pair_id)) {
    return false;
  }
  struct mgp_map *map = NULL;
  if (!mg_ok(mgp_unordered_map_make_empty(memory, &map)) || map == NULL) {
    return false;
  }
  const bool populated =
      map_insert_int(map, "pair_id", pair_id, memory) &&
      map_insert_int(map, "oracle_epoch", epoch, memory) &&
      map_insert_int(map, "updated_generation", generation, memory) &&
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
                               const sg_pair_oracle *oracle, uint64_t oracle_epoch,
                               bool create_predecessors, size_t first_edge, size_t edge_count) {
  static const char *next_query =
      "UNWIND $edges AS e "
      "MATCH (p:SyncPair {model: $model, oracle_epoch: e.oracle_epoch, pair_id: e.pair_id}), "
      "(n:SyncPair {model: $model, oracle_epoch: e.oracle_epoch, pair_id: e.next_pair}) "
      "CREATE (p)-[:PAIR_NEXT {model: $model, oracle_epoch: e.oracle_epoch, "
      "updated_generation: e.updated_generation, action: e.action, "
      "action_id: e.action_id, outputs_differ: e.outputs_differ}]->(n)";
  static const char *next_and_pre_query =
      "UNWIND $edges AS e "
      "MATCH (p:SyncPair {model: $model, oracle_epoch: e.oracle_epoch, pair_id: e.pair_id}), "
      "(n:SyncPair {model: $model, oracle_epoch: e.oracle_epoch, pair_id: e.next_pair}) "
      "CREATE (p)-[:PAIR_NEXT {model: $model, oracle_epoch: e.oracle_epoch, "
      "updated_generation: e.updated_generation, action: e.action, "
      "action_id: e.action_id, outputs_differ: e.outputs_differ}]->(n) "
      "CREATE (n)-[:PAIR_PRE {model: $model, oracle_epoch: e.oracle_epoch, "
      "updated_generation: e.updated_generation, action: e.action, "
      "action_id: e.action_id, outputs_differ: e.outputs_differ}]->(p)";
  struct mgp_list *edges = NULL;
  if (!mg_ok(mgp_list_make_empty(edge_count, memory, &edges)) || edges == NULL) {
    return false;
  }
  bool ok = true;
  for (size_t offset = 0U; ok && offset < edge_count; ++offset) {
    ok = append_pair_edge(edges, automaton, oracle, oracle_epoch, first_edge + offset, memory);
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
  if (!params_insert_value(params, "edges", edges_value)) {
    if (params != NULL) {
      mgp_map_destroy(params);
    }
    return false;
  }
  ok = execute_drain(graph, memory, create_predecessors ? next_and_pre_query : next_query, params);
  mgp_map_destroy(params);
  return ok;
}

static bool materialize_oracle(struct mgp_graph *graph, struct mgp_memory *memory,
                               const char *model, const sg_automaton *automaton,
                               const sg_pair_oracle *oracle, uint64_t oracle_epoch,
                               bool materialize_edges, bool incremental,
                               const char *snapshot_token) {
  static const char *clear_query = "MATCH (p:SyncPair {model: $model}) DETACH DELETE p";
  static const char *finish_query = "MATCH (m:SyncModel {model: $model}) "
                                    "SET m.dirty = false, m.prepared_generation = $generation, "
                                    "m.oracle_epoch = $oracle_epoch, "
                                    "m.incremental = $incremental, "
                                    "m.pair_edges_materialized = $pair_edges_materialized, "
                                    "m.snapshot_token = $snapshot_token";
  if (!execute_model_query(graph, memory, model, clear_query)) {
    return false;
  }
  const size_t pair_count = sg_pair_oracle_pair_count(oracle);
  for (size_t first = 0U; first < pair_count; first += SG_MATERIALIZE_BATCH) {
    const size_t remaining = pair_count - first;
    const size_t count = remaining < SG_MATERIALIZE_BATCH ? remaining : SG_MATERIALIZE_BATCH;
    if (!execute_pair_batch(graph, memory, model, automaton, oracle, oracle_epoch, first, count)) {
      return false;
    }
  }
  if (materialize_edges) {
    const size_t edge_count = sg_pair_oracle_pair_edge_count(oracle);
    for (size_t first = 0U; first < edge_count; first += SG_MATERIALIZE_BATCH) {
      const size_t remaining = edge_count - first;
      const size_t count = remaining < SG_MATERIALIZE_BATCH ? remaining : SG_MATERIALIZE_BATCH;
      if (!execute_edge_batch(graph, memory, model, automaton, oracle, oracle_epoch,
                              materialize_edges, first, count)) {
        return false;
      }
    }
  }
  struct mgp_map *params = make_model_params(model, memory);
  int64_t generation = 0;
  int64_t epoch = 0;
  if (params == NULL || !uint64_to_int64(sg_automaton_generation(automaton), &generation) ||
      !uint64_to_int64(oracle_epoch, &epoch) ||
      !params_insert_int(params, "generation", generation, memory) ||
      !params_insert_int(params, "oracle_epoch", epoch, memory) ||
      !params_insert_bool(params, "incremental", incremental, memory) ||
      !params_insert_bool(params, "pair_edges_materialized", materialize_edges, memory) ||
      !params_insert_string(params, "snapshot_token", snapshot_token, memory)) {
    if (params != NULL) {
      mgp_map_destroy(params);
    }
    return false;
  }
  const bool ok = execute_drain(graph, memory, finish_query, params);
  mgp_map_destroy(params);
  return ok;
}

static bool load_prepared_automaton(struct mgp_graph *graph, struct mgp_memory *memory,
                                    const char *model, model_metadata *metadata,
                                    sg_automaton **automaton, struct mgp_result *result) {
  const sg_status status = load_automaton(graph, memory, model, metadata, automaton);
  if (status != SG_OK) {
    set_status_error(result, "model extraction failed", status);
    return false;
  }
  if (!metadata->prepared) {
    set_error(result, "model is dirty or has not been prepared");
    sg_automaton_free(*automaton);
    *automaton = NULL;
    return false;
  }
  return true;
}

static sg_status load_snapshot_record_batch(struct mgp_graph *graph, struct mgp_memory *memory,
                                            const char *model, uint64_t oracle_epoch,
                                            size_t first_pair, size_t pair_count,
                                            sg_pair_record *records) {
  static const char *query =
      "MATCH (p:SyncPair {model: $model, oracle_epoch: $oracle_epoch}) "
      "WHERE p.pair_id >= $first_pair AND p.pair_id < $last_pair "
      "RETURN p.pair_id AS pair_id, p.mergeable AS mergeable, "
      "p.merge_distance AS merge_distance, p.merge_action_id AS merge_action_id, "
      "p.merge_next_pair AS merge_next_pair, p.merge_support_count AS merge_support_count, "
      "p.resolvable AS resolvable, p.resolution_distance AS resolution_distance, "
      "p.resolution_action_id AS resolution_action_id, "
      "p.resolution_next_pair AS resolution_next_pair, "
      "p.resolution_support_count AS resolution_support_count "
      "ORDER BY p.pair_id";
  struct mgp_map *params = make_model_params(model, memory);
  int64_t epoch = 0;
  int64_t first = 0;
  int64_t last = 0;
  if (params == NULL || !uint64_to_int64(oracle_epoch, &epoch) ||
      !size_to_int64(first_pair, &first) || !size_to_int64(first_pair + pair_count, &last) ||
      !params_insert_int(params, "oracle_epoch", epoch, memory) ||
      !params_insert_int(params, "first_pair", first, memory) ||
      !params_insert_int(params, "last_pair", last, memory)) {
    if (params != NULL) {
      mgp_map_destroy(params);
    }
    return SG_ERR_ALLOC;
  }
  struct mgp_execution_result *execution = NULL;
  sg_status status = SG_OK;
  if (!mg_ok(mgp_execute_query(graph, memory, query, params, &execution)) || execution == NULL) {
    status = SG_ERR_INVALID_MODEL;
  }
  size_t loaded = 0U;
  while (status == SG_OK) {
    struct mgp_map *row = NULL;
    if (!mg_ok(mgp_pull_one(execution, graph, memory, &row))) {
      status = SG_ERR_INVALID_MODEL;
      break;
    }
    if (row == NULL) {
      break;
    }
    if (loaded >= pair_count || !row_pair_record(row, &records[loaded]) ||
        records[loaded].pair != first_pair + loaded) {
      status = SG_ERR_INVALID_MODEL;
      break;
    }
    ++loaded;
  }
  if (execution != NULL) {
    mgp_execution_result_destroy(execution);
  }
  mgp_map_destroy(params);
  return status == SG_OK && loaded == pair_count ? SG_OK : SG_ERR_INVALID_MODEL;
}

static sg_status hydrate_snapshot(struct mgp_graph *graph, struct mgp_memory *memory,
                                  const char *model, const model_metadata *metadata,
                                  planning_metrics *metrics, sg_pair_snapshot **snapshot) {
  const uint64_t start = monotonic_time_us();
  sg_automaton *automaton = NULL;
  sg_status status =
      load_automaton_generation(graph, memory, model, metadata->generation, &automaton);
  size_t pair_count = 0U;
  if (status == SG_OK && !pair_count_for_states(sg_automaton_state_count(automaton), &pair_count)) {
    status = SG_ERR_ALLOC;
  }
  sg_pair_record *records = NULL;
  if (status == SG_OK) {
    records = calloc(pair_count == 0U ? 1U : pair_count, sizeof(*records));
    if (records == NULL) {
      status = SG_ERR_ALLOC;
    }
  }
  for (size_t first = 0U; status == SG_OK && first < pair_count; first += SG_HYDRATE_BATCH) {
    const size_t remaining = pair_count - first;
    const size_t count = remaining < SG_HYDRATE_BATCH ? remaining : SG_HYDRATE_BATCH;
    status = load_snapshot_record_batch(graph, memory, model, metadata->oracle_epoch, first, count,
                                        &records[first]);
    ++metrics->oracle_load_batches;
    if (status == SG_OK) {
      metrics->oracle_rows_loaded += count;
    }
  }
  if (status == SG_OK) {
    status = sg_pair_snapshot_restore(automaton, records, pair_count, snapshot);
  }
  free(records);
  sg_automaton_free(automaton);
  metrics->snapshot_hydration_time_us = elapsed_us(start);
  metrics->oracle_time_us += metrics->snapshot_hydration_time_us;
  return status;
}

static bool load_prepared_snapshot(struct mgp_graph *graph, struct mgp_memory *memory,
                                   const char *model, model_metadata *metadata,
                                   planning_metrics *metrics, sg_pair_snapshot **snapshot,
                                   struct mgp_result *result) {
  sg_status status = load_metadata(graph, memory, model, metadata);
  if (status != SG_OK) {
    set_status_error(result, "model metadata lookup failed", status);
    return false;
  }
  if (!metadata->prepared) {
    set_error(result, "model is dirty or has not been prepared for this module version");
    return false;
  }
  *snapshot = sg_snapshot_cache_lookup(snapshot_cache, model, metadata->oracle_epoch,
                                       metadata->generation, metadata->snapshot_token);
  if (*snapshot != NULL) {
    metrics->cache = CACHE_STATE_HOT;
    return true;
  }
  metrics->cache = CACHE_STATE_HYDRATED;
  status = hydrate_snapshot(graph, memory, model, metadata, metrics, snapshot);
  bool stored = false;
  if (status == SG_OK) {
    status = sg_snapshot_cache_insert(snapshot_cache, model, metadata->oracle_epoch,
                                      metadata->generation, metadata->snapshot_token, *snapshot,
                                      &stored);
  }
  (void)stored;
  if (status != SG_OK) {
    set_status_error(result, "prepared snapshot hydration failed", status);
    sg_pair_snapshot_release(*snapshot);
    *snapshot = NULL;
    return false;
  }
  return true;
}

static sg_status snapshot_record_read(void *raw_context, const size_t *pair_ids, size_t pair_count,
                                      sg_pair_record *records) {
  snapshot_record_context *context = raw_context;
  const sg_status status = sg_pair_snapshot_read(context->snapshot, pair_ids, pair_count, records);
  if (status == SG_OK) {
    context->metrics->snapshot_record_reads += pair_count;
    context->metrics->oracle_cache_hits += pair_count;
  }
  return status;
}

static void prepare_model_cb(struct mgp_list *arguments, struct mgp_graph *graph,
                             struct mgp_result *result, struct mgp_memory *memory) {
  const char *model = NULL;
  bool materialize_edges = false;
  bool incremental = false;
  if (!get_string_arg(arguments, 0U, &model) ||
      !get_bool_arg_default(arguments, 1U, false, &materialize_edges) ||
      !get_bool_arg_default(arguments, 2U, false, &incremental)) {
    set_error(result, "expected model and optional materialize/incremental booleans");
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
  sg_pair_snapshot *prepared_snapshot = NULL;
  if (status == SG_OK) {
    status = sg_pair_snapshot_from_oracle(automaton, oracle, &prepared_snapshot);
  }
  const uint64_t oracle_epoch = metadata.oracle_epoch + 1U;
  const size_t pair_count = status == SG_OK ? sg_pair_oracle_pair_count(oracle) : 0U;
  const size_t edge_count = status == SG_OK ? sg_pair_oracle_pair_edge_count(oracle) : 0U;
  int64_t ignored = 0;
  if (status == SG_OK && (oracle_epoch == 0U || !size_to_int64(pair_count, &ignored) ||
                          !size_to_int64(sg_automaton_action_count(automaton), &ignored))) {
    status = SG_ERR_RESOURCE_BOUND;
  }
  char snapshot_token[SG_SNAPSHOT_TOKEN_CAPACITY] = {0};
  if (status == SG_OK && !make_snapshot_token(snapshot_token)) {
    status = SG_ERR_RESOURCE_BOUND;
  }
  if (status == SG_OK && !materialize_oracle(graph, memory, model, automaton, oracle, oracle_epoch,
                                             materialize_edges, incremental, snapshot_token)) {
    status = SG_ERR_INVALID_MODEL;
  }
  bool snapshot_stored = false;
  if (status == SG_OK) {
    status = sg_snapshot_cache_insert(snapshot_cache, model, oracle_epoch,
                                      sg_automaton_generation(automaton), snapshot_token,
                                      prepared_snapshot, &snapshot_stored);
  }
  if (status != SG_OK) {
    set_status_error(result, "model preparation failed", status);
    sg_pair_snapshot_release(prepared_snapshot);
    sg_pair_oracle_free(oracle);
    sg_automaton_free(automaton);
    return;
  }
  struct mgp_result_record *record = NULL;
  int64_t generation = 0;
  int64_t epoch = 0;
  int64_t states = 0;
  int64_t actions = 0;
  int64_t outputs = 0;
  int64_t transitions = 0;
  int64_t pairs = 0;
  int64_t edges = 0;
  const bool converted = uint64_to_int64(sg_automaton_generation(automaton), &generation) &&
                         uint64_to_int64(oracle_epoch, &epoch) &&
                         size_to_int64(sg_automaton_state_count(automaton), &states) &&
                         size_to_int64(sg_automaton_action_count(automaton), &actions) &&
                         size_to_int64(sg_automaton_output_count(automaton), &outputs) &&
                         size_to_int64(sg_automaton_transition_count(automaton), &transitions) &&
                         size_to_int64(pair_count, &pairs) && size_to_int64(edge_count, &edges);
  if (!converted || !new_record(result, &record) ||
      !insert_string(record, "status", "OK", memory) ||
      !insert_int(record, "generation", generation, memory) ||
      !insert_int(record, "oracle_epoch", epoch, memory) ||
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
      !insert_bool(record, "materialized_pair_edges", materialize_edges, memory) ||
      !insert_bool(record, "incremental_enabled", incremental, memory)) {
    set_error(result, "failed to create preparation result");
  }
  (void)snapshot_stored;
  sg_pair_snapshot_release(prepared_snapshot);
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);
}

static bool insert_plan_common(struct mgp_result_record *record, const sg_automaton *automaton,
                               const sg_plan_result *plan, const planning_metrics *metrics,
                               struct mgp_memory *memory) {
  int64_t generation = 0;
  int64_t length = 0;
  int64_t expansions = 0;
  int64_t planning_time = 0;
  int64_t oracle_builds = 0;
  int64_t oracle_rows_loaded = 0;
  int64_t oracle_load_batches = 0;
  int64_t oracle_cache_hits = 0;
  int64_t snapshot_record_reads = 0;
  int64_t oracle_time = 0;
  int64_t snapshot_hydration_time = 0;
  int64_t total_compute_time = 0;
  return uint64_to_int64(plan->generation, &generation) &&
         size_to_int64(plan->word.length, &length) &&
         size_to_int64(plan->expansions, &expansions) &&
         uint64_to_int64(plan->planning_time_us, &planning_time) &&
         size_to_int64(metrics->oracle_builds, &oracle_builds) &&
         size_to_int64(metrics->oracle_rows_loaded, &oracle_rows_loaded) &&
         size_to_int64(metrics->oracle_load_batches, &oracle_load_batches) &&
         size_to_int64(metrics->oracle_cache_hits, &oracle_cache_hits) &&
         size_to_int64(metrics->snapshot_record_reads, &snapshot_record_reads) &&
         uint64_to_int64(metrics->oracle_time_us, &oracle_time) &&
         uint64_to_int64(metrics->snapshot_hydration_time_us, &snapshot_hydration_time) &&
         uint64_to_int64(metrics->total_compute_time_us, &total_compute_time) &&
         insert_string(record, "status", "OK", memory) &&
         insert_string(record, "outcome", sg_plan_outcome_name(plan->outcome), memory) &&
         insert_string(record, "reason", outcome_reason(plan->outcome), memory) &&
         insert_string(record, "method", sg_plan_method_name(plan->method), memory) &&
         insert_word(record, automaton, &plan->word, memory) &&
         insert_int(record, "length", length, memory) &&
         insert_int(record, "expansions", expansions, memory) &&
         insert_int(record, "generation", generation, memory) &&
         insert_int(record, "planning_time_us", planning_time, memory) &&
         insert_string(record, "oracle_source", oracle_source_name(metrics->source), memory) &&
         insert_string(record, "cache_state", cache_state_name(metrics->cache), memory) &&
         insert_int(record, "oracle_builds", oracle_builds, memory) &&
         insert_int(record, "oracle_rows_loaded", oracle_rows_loaded, memory) &&
         insert_int(record, "oracle_load_batches", oracle_load_batches, memory) &&
         insert_int(record, "oracle_cache_hits", oracle_cache_hits, memory) &&
         insert_int(record, "snapshot_record_reads", snapshot_record_reads, memory) &&
         insert_int(record, "oracle_time_us", oracle_time, memory) &&
         insert_int(record, "snapshot_hydration_time_us", snapshot_hydration_time, memory) &&
         insert_int(record, "total_compute_time_us", total_compute_time, memory);
}

static void plan_sync_impl(struct mgp_list *arguments, struct mgp_graph *graph,
                           struct mgp_result *result, struct mgp_memory *memory,
                           oracle_source source) {
  const char *model = NULL;
  int64_t budget = 0;
  if (!get_string_arg(arguments, 0U, &model) || !get_int_arg(arguments, 2U, &budget) ||
      budget <= 0 || (uint64_t)budget > (uint64_t)SIZE_MAX) {
    set_error(result, "expected model, nonempty hypotheses, and positive budget");
    return;
  }
  const uint64_t total_start = monotonic_time_us();
  sg_automaton *owned_automaton = NULL;
  const sg_automaton *automaton = NULL;
  sg_pair_oracle *oracle = NULL;
  sg_pair_snapshot *snapshot = NULL;
  model_metadata metadata = {0};
  planning_metrics metrics = {.source = source, .cache = CACHE_STATE_BYPASSED};
  if (source == ORACLE_SOURCE_PERSISTED) {
    if (!load_prepared_snapshot(graph, memory, model, &metadata, &metrics, &snapshot, result)) {
      return;
    }
    automaton = sg_pair_snapshot_automaton(snapshot);
  } else {
    const sg_status load_status = load_base_automaton(graph, memory, model, &owned_automaton);
    if (load_status != SG_OK) {
      set_status_error(result, "model extraction failed", load_status);
      return;
    }
    automaton = owned_automaton;
  }
  size_t *hypotheses = NULL;
  size_t hypothesis_count = 0U;
  if (!argument_to_ids(automaton, arguments, 1U, false, false, &hypotheses, &hypothesis_count)) {
    set_error(result, "hypotheses must be a nonempty list of known state keys");
    sg_pair_oracle_free(oracle);
    sg_pair_snapshot_release(snapshot);
    sg_automaton_free(owned_automaton);
    return;
  }
  sg_plan_result plan = {0};
  snapshot_record_context snapshot_records = {
      .snapshot = snapshot,
      .metrics = &metrics,
  };
  sg_status status = SG_OK;
  if (source == ORACLE_SOURCE_PERSISTED) {
    const sg_pair_record_source record_source = {
        .context = &snapshot_records,
        .read = snapshot_record_read,
    };
    status = sg_plan_sync_from_records(automaton, &record_source, hypotheses, hypothesis_count,
                                       (size_t)budget, &plan);
  } else {
    const uint64_t oracle_start = monotonic_time_us();
    metrics.oracle_builds = 1U;
    status = sg_pair_oracle_build(automaton, &oracle);
    metrics.oracle_time_us = elapsed_us(oracle_start);
    if (status == SG_OK) {
      status = sg_plan_sync(automaton, oracle, hypotheses, hypothesis_count, (size_t)budget, &plan);
    }
  }
  metrics.total_compute_time_us = elapsed_us(total_start);
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
        !new_record(result, &record) ||
        !insert_plan_common(record, automaton, &plan, &metrics, memory) ||
        !insert_string(record, "final_state_key", final_state, memory) ||
        !insert_int(record, "final_support_size", final_support, memory)) {
      set_error(result, "failed to create synchronization result");
    }
  }
  sg_plan_result_free(&plan);
  sg_pair_oracle_free(oracle);
  sg_pair_snapshot_release(snapshot);
  sg_automaton_free(owned_automaton);
}

static void plan_sync_cb(struct mgp_list *arguments, struct mgp_graph *graph,
                         struct mgp_result *result, struct mgp_memory *memory) {
  plan_sync_impl(arguments, graph, result, memory, ORACLE_SOURCE_PERSISTED);
}

static void plan_sync_uncached_cb(struct mgp_list *arguments, struct mgp_graph *graph,
                                  struct mgp_result *result, struct mgp_memory *memory) {
  plan_sync_impl(arguments, graph, result, memory, ORACLE_SOURCE_RECOMPUTED);
}


/* Action-restricted planning and goal planning.
 *
 * Additive: upstream's plan_sync, plan_disambiguate and their _uncached
 * variants keep their names, arguments and behaviour. These three procedures
 * are what a robot needs - a word it can only build from letters it can
 * actually execute, and somewhere to go. Both restricted planners are
 * automaton-only, so they load the prepared automaton and never pay for a
 * pair oracle or a snapshot. */
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
  model_metadata metadata = {0};
  if (!load_prepared_automaton(graph, memory, model, &metadata, &automaton, result)) {
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
    sg_automaton_free(automaton);
    return;
  }
  sg_plan_result plan = {0};
  planning_metrics metrics = {.source = ORACLE_SOURCE_PERSISTED, .cache = CACHE_STATE_BYPASSED};
  const sg_status status = sg_plan_sync_allowed(automaton, hypotheses, hypothesis_count, actions,
                                                action_count, (size_t)budget, &plan);
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
        !new_record(result, &record) ||
        !insert_plan_common(record, automaton, &plan, &metrics, memory) ||
        !insert_string(record, "final_state_key", final_state, memory) ||
        !insert_int(record, "final_support_size", final_support, memory)) {
      set_error(result, "failed to create restricted synchronization result");
    }
  }
  sg_plan_result_free(&plan);
  sg_automaton_free(automaton);
}

static void plan_disambiguate_impl(struct mgp_list *arguments, struct mgp_graph *graph,
                                   struct mgp_result *result, struct mgp_memory *memory,
                                   oracle_source source, bool restricted) {
  /* `restricted` shifts the argument layout: the restricted procedure takes
   * the allowed alphabet at index 3 and its budget at 4, upstream's takes the
   * budget at 3 and has no alphabet. Everything after that is identical, which
   * is why this is one implementation rather than two. */
  const size_t budget_index = restricted ? 4U : 3U;
  const char *model = NULL;
  int64_t bound = 0;
  int64_t budget = 0;
  if (!get_string_arg(arguments, 0U, &model) || !get_int_arg(arguments, 2U, &bound) ||
      !get_int_arg(arguments, budget_index, &budget) || bound <= 0 || budget <= 0 ||
      (uint64_t)bound > (uint64_t)SIZE_MAX || (uint64_t)budget > (uint64_t)SIZE_MAX) {
    set_error(result, "expected model, nonempty hypotheses, positive bound, and positive budget");
    return;
  }
  const uint64_t total_start = monotonic_time_us();
  sg_automaton *owned_automaton = NULL;
  const sg_automaton *automaton = NULL;
  sg_pair_oracle *oracle = NULL;
  sg_pair_snapshot *snapshot = NULL;
  model_metadata metadata = {0};
  planning_metrics metrics = {.source = source, .cache = CACHE_STATE_BYPASSED};
  if (source == ORACLE_SOURCE_PERSISTED) {
    if (!load_prepared_snapshot(graph, memory, model, &metadata, &metrics, &snapshot, result)) {
      return;
    }
    automaton = sg_pair_snapshot_automaton(snapshot);
  } else {
    const sg_status load_status = load_base_automaton(graph, memory, model, &owned_automaton);
    if (load_status != SG_OK) {
      set_status_error(result, "model extraction failed", load_status);
      return;
    }
    automaton = owned_automaton;
  }
  size_t *hypotheses = NULL;
  size_t hypothesis_count = 0U;
  size_t *actions = NULL;
  size_t action_count = 0U;
  if (!argument_to_ids(automaton, arguments, 1U, false, false, &hypotheses, &hypothesis_count) ||
      (restricted &&
       !argument_to_ids(automaton, arguments, 3U, true, false, &actions, &action_count))) {
    free(actions);
    set_error(result, "hypotheses or actions contain unknown prepared keys");
    sg_pair_oracle_free(oracle);
    sg_pair_snapshot_release(snapshot);
    sg_automaton_free(owned_automaton);
    return;
  }
  sg_plan_result plan = {0};
  snapshot_record_context snapshot_records = {
      .snapshot = snapshot,
      .metrics = &metrics,
  };
  sg_status status = SG_OK;
  if (source == ORACLE_SOURCE_PERSISTED) {
    const sg_pair_record_source record_source = {
        .context = &snapshot_records,
        .read = snapshot_record_read,
    };
    status = restricted ? sg_plan_disambiguate_allowed_from_records(
                              automaton, &record_source, hypotheses, hypothesis_count,
                              (size_t)bound, actions, action_count, (size_t)budget, &plan)
                        : sg_plan_disambiguate_from_records(automaton, &record_source, hypotheses,
                                                            hypothesis_count, (size_t)bound,
                                                            (size_t)budget, &plan);
  } else {
    const uint64_t oracle_start = monotonic_time_us();
    metrics.oracle_builds = 1U;
    status = sg_pair_oracle_build(automaton, &oracle);
    metrics.oracle_time_us = elapsed_us(oracle_start);
    if (status == SG_OK) {
      status = restricted
                   ? sg_plan_disambiguate_allowed(automaton, oracle, hypotheses, hypothesis_count,
                                                  (size_t)bound, actions, action_count,
                                                  (size_t)budget, &plan)
                   : sg_plan_disambiguate(automaton, oracle, hypotheses, hypothesis_count,
                                          (size_t)bound, (size_t)budget, &plan);
    }
  }
  metrics.total_compute_time_us = elapsed_us(total_start);
  free(hypotheses);
  free(actions);
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
        !insert_plan_common(record, automaton, &plan, &metrics, memory) ||
        !insert_int(record, "best_support_size", best, memory) ||
        !insert_int(record, "worst_support_size", worst, memory) ||
        !insert_int(record, "branch_count", branches, memory) ||
        !insert_bool(record, "homing", plan.homing, memory)) {
      set_error(result, "failed to create disambiguation result");
    }
  }
  sg_plan_result_free(&plan);
  sg_pair_oracle_free(oracle);
  sg_pair_snapshot_release(snapshot);
  sg_automaton_free(owned_automaton);
}

static void plan_disambiguate_cb(struct mgp_list *arguments, struct mgp_graph *graph,
                                 struct mgp_result *result, struct mgp_memory *memory) {
  plan_disambiguate_impl(arguments, graph, result, memory, ORACLE_SOURCE_PERSISTED, false);
}

static void plan_disambiguate_uncached_cb(struct mgp_list *arguments, struct mgp_graph *graph,
                                          struct mgp_result *result, struct mgp_memory *memory) {
  plan_disambiguate_impl(arguments, graph, result, memory, ORACLE_SOURCE_RECOMPUTED, false);
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
  model_metadata metadata = {0};
  if (!load_prepared_automaton(graph, memory, model, &metadata, &automaton, result)) {
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
  if (!load_prepared_automaton(graph, memory, model, &metadata, &automaton, result)) {
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

static sg_status persist_snapshot_records(memgraph_pair_store *store,
                                          const sg_pair_snapshot *snapshot, bool write_all,
                                          size_t *db_write_batches) {
  sg_pair_record *records = calloc(SG_MATERIALIZE_BATCH, sizeof(*records));
  if (records == NULL) {
    return SG_ERR_ALLOC;
  }
  const size_t record_count =
      write_all ? sg_pair_snapshot_pair_count(snapshot) : sg_pair_snapshot_changed_count(snapshot);
  const size_t *changed_pairs = sg_pair_snapshot_changed_pairs(snapshot);
  sg_status status = SG_OK;
  for (size_t first = 0U; status == SG_OK && first < record_count; first += SG_MATERIALIZE_BATCH) {
    const size_t remaining = record_count - first;
    const size_t count = remaining < SG_MATERIALIZE_BATCH ? remaining : SG_MATERIALIZE_BATCH;
    for (size_t offset = 0U; status == SG_OK && offset < count; ++offset) {
      const size_t pair = write_all ? first + offset : changed_pairs[first + offset];
      status = sg_pair_snapshot_record(snapshot, pair, &records[offset]);
    }
    if (status == SG_OK) {
      status = memgraph_store_write_record_batch(store, records, count);
      if (status == SG_OK) {
        ++*db_write_batches;
      }
    }
  }
  free(records);
  return status;
}

static bool finish_incremental_update(memgraph_pair_store *store, const char *snapshot_token) {
  static const char *query = "MATCH (m:SyncModel {model: $model, oracle_epoch: $oracle_epoch}) "
                             "SET m.generation = $generation, m.prepared_generation = $generation, "
                             "m.dirty = false, m.incremental = true, "
                             "m.snapshot_token = $snapshot_token";
  struct mgp_map *params = make_model_params(store->model, store->memory);
  int64_t epoch = 0;
  int64_t generation = 0;
  if (params == NULL || !uint64_to_int64(store->oracle_epoch, &epoch) ||
      !uint64_to_int64(store->updated_generation, &generation) ||
      !params_insert_int(params, "oracle_epoch", epoch, store->memory) ||
      !params_insert_int(params, "generation", generation, store->memory) ||
      !params_insert_string(params, "snapshot_token", snapshot_token, store->memory)) {
    if (params != NULL) {
      mgp_map_destroy(params);
    }
    return false;
  }
  const bool ok = execute_drain(store->graph, store->memory, query, params);
  mgp_map_destroy(params);
  return ok;
}

static bool insert_update_result(struct mgp_result *result, const char *status,
                                 const char *maintenance_mode, uint64_t generation,
                                 uint64_t oracle_epoch, size_t changed_cells,
                                 size_t direct_pair_edges, const sg_pair_repair_metrics *metrics,
                                 size_t db_write_batches, bool fallback_rebuild,
                                 uint64_t maintenance_time_us, struct mgp_memory *memory) {
  struct mgp_result_record *record = NULL;
  int64_t converted_generation = 0;
  int64_t converted_epoch = 0;
  int64_t converted_cells = 0;
  int64_t converted_edges = 0;
  int64_t converted_touched = 0;
  int64_t converted_examined = 0;
  int64_t converted_written = 0;
  int64_t converted_edges_examined = 0;
  int64_t converted_write_batches = 0;
  int64_t converted_merge_changed = 0;
  int64_t converted_merge_invalidated = 0;
  int64_t converted_resolution_changed = 0;
  int64_t converted_resolution_invalidated = 0;
  int64_t converted_time = 0;
  return uint64_to_int64(generation, &converted_generation) &&
         uint64_to_int64(oracle_epoch, &converted_epoch) &&
         size_to_int64(changed_cells, &converted_cells) &&
         size_to_int64(direct_pair_edges, &converted_edges) &&
         size_to_int64(metrics->pair_records_touched, &converted_touched) &&
         size_to_int64(metrics->pair_records_examined, &converted_examined) &&
         size_to_int64(metrics->pair_records_written, &converted_written) &&
         size_to_int64(metrics->pair_edges_examined, &converted_edges_examined) &&
         size_to_int64(db_write_batches, &converted_write_batches) &&
         size_to_int64(metrics->merge_pairs_changed, &converted_merge_changed) &&
         size_to_int64(metrics->merge_pairs_invalidated, &converted_merge_invalidated) &&
         size_to_int64(metrics->resolution_pairs_changed, &converted_resolution_changed) &&
         size_to_int64(metrics->resolution_pairs_invalidated, &converted_resolution_invalidated) &&
         uint64_to_int64(maintenance_time_us, &converted_time) && new_record(result, &record) &&
         insert_string(record, "status", status, memory) &&
         insert_string(record, "maintenance_mode", maintenance_mode, memory) &&
         insert_int(record, "generation", converted_generation, memory) &&
         insert_int(record, "oracle_epoch", converted_epoch, memory) &&
         insert_int(record, "changed_cells", converted_cells, memory) &&
         insert_int(record, "direct_pair_edges", converted_edges, memory) &&
         insert_int(record, "pair_records_touched", converted_touched, memory) &&
         insert_int(record, "pair_records_examined", converted_examined, memory) &&
         insert_int(record, "pair_records_written", converted_written, memory) &&
         insert_int(record, "pair_edges_examined", converted_edges_examined, memory) &&
         insert_int(record, "db_write_batches", converted_write_batches, memory) &&
         insert_int(record, "merge_pairs_changed", converted_merge_changed, memory) &&
         insert_int(record, "merge_pairs_invalidated", converted_merge_invalidated, memory) &&
         insert_int(record, "resolution_pairs_changed", converted_resolution_changed, memory) &&
         insert_int(record, "resolution_pairs_invalidated", converted_resolution_invalidated,
                    memory) &&
         insert_bool(record, "fallback_rebuild", fallback_rebuild, memory) &&
         insert_int(record, "maintenance_time_us", converted_time, memory);
}

static void set_full_rebuild_metrics(sg_pair_repair_metrics *metrics, size_t pair_count,
                                     size_t action_count) {
  metrics->pair_records_touched = pair_count;
  metrics->pair_records_examined = pair_count;
  metrics->merge_pairs_changed = pair_count;
  metrics->resolution_pairs_changed = pair_count;
  metrics->pair_edges_examined =
      action_count > SIZE_MAX / pair_count ? SIZE_MAX : pair_count * action_count;
}

static void update_cells_cb(struct mgp_list *arguments, struct mgp_graph *graph,
                            struct mgp_result *result, struct mgp_memory *memory) {
  const uint64_t start = monotonic_time_us();
  const char *model = NULL;
  int64_t requested_budget = 0;
  if (!get_string_arg(arguments, 0U, &model) ||
      !get_int_arg_default(arguments, 2U, 0, &requested_budget) || requested_budget < -1 ||
      (requested_budget > 0 && (uint64_t)requested_budget > (uint64_t)SIZE_MAX)) {
    set_error(result, "expected model, cell-change maps, and repair budget -1 or greater");
    return;
  }
  planning_metrics snapshot_metrics = {0};
  model_metadata metadata = {0};
  sg_pair_snapshot *base_snapshot = NULL;
  if (!load_prepared_snapshot(graph, memory, model, &metadata, &snapshot_metrics, &base_snapshot,
                              result)) {
    return;
  }
  if (!metadata.incremental) {
    set_error(result, "model was not prepared for incremental maintenance");
    sg_pair_snapshot_release(base_snapshot);
    return;
  }
  const sg_automaton *old_automaton = sg_pair_snapshot_automaton(base_snapshot);
  cell_change *changes = NULL;
  size_t change_count = 0U;
  if (!parse_cell_changes(old_automaton, arguments, &changes, &change_count)) {
    set_error(result, "changes must be unique valid state/action maps with target and/or output");
    sg_pair_snapshot_release(base_snapshot);
    return;
  }
  sg_pair_repair_metrics metrics = {0};
  if (change_count == 0U) {
    if (!insert_update_result(result, "UNCHANGED", "UNCHANGED", metadata.generation,
                              metadata.oracle_epoch, 0U, 0U, &metrics, 0U, false, elapsed_us(start),
                              memory)) {
      set_error(result, "failed to create unchanged update result");
    }
    free(changes);
    sg_pair_snapshot_release(base_snapshot);
    return;
  }
  if (metadata.generation == UINT64_MAX) {
    set_error(result, "model generation is exhausted");
    free(changes);
    sg_pair_snapshot_release(base_snapshot);
    return;
  }
  const uint64_t new_generation = metadata.generation + 1U;
  sg_pair_snapshot *candidate = NULL;
  sg_status status = sg_pair_snapshot_clone(base_snapshot, new_generation, &candidate);
  for (size_t index = 0U; status == SG_OK && index < change_count; ++index) {
    bool changed = false;
    status = sg_pair_snapshot_set_cell(candidate, changes[index].state, changes[index].action,
                                       changes[index].target, changes[index].output, &changed);
    if (status == SG_OK && !changed) {
      status = SG_ERR_INVALID_MODEL;
    }
  }
  if (status != SG_OK) {
    set_status_error(result, "updated snapshot construction failed", status);
    free(changes);
    sg_pair_snapshot_release(candidate);
    sg_pair_snapshot_release(base_snapshot);
    return;
  }
  const sg_automaton *automaton = sg_pair_snapshot_automaton(candidate);
  pair_edge_update *updates = NULL;
  size_t update_count = 0U;
  size_t *seed_pairs = NULL;
  size_t seed_count = 0U;
  status = build_edge_updates(automaton, changes, change_count, &updates, &update_count,
                              &seed_pairs, &seed_count);
  memgraph_pair_store store_context = {
      .graph = graph,
      .memory = memory,
      .model = model,
      .oracle_epoch = metadata.oracle_epoch,
      .updated_generation = new_generation,
      .automaton = automaton,
  };
  const size_t pair_count = sg_pair_snapshot_pair_count(candidate);
  const size_t automatic_budget = (pair_count / 4U) + (pair_count % 4U != 0U ? 1U : 0U);
  const size_t repair_budget = requested_budget == 0 ? automatic_budget : (size_t)requested_budget;
  bool fallback_rebuild = false;
  bool full_rebuild = requested_budget == -1;
  if (status == SG_OK && !full_rebuild) {
    status = sg_pair_snapshot_repair(candidate, seed_pairs, seed_count, repair_budget, &metrics);
  }
  if (status == SG_ERR_RESOURCE_BOUND) {
    fallback_rebuild = true;
    full_rebuild = true;
    status = SG_OK;
  }
  if (status == SG_OK && full_rebuild) {
    sg_pair_snapshot *rebuilt = NULL;
    status = sg_pair_snapshot_build(sg_pair_snapshot_automaton(candidate), &rebuilt);
    if (status == SG_OK) {
      sg_pair_snapshot_release(candidate);
      candidate = rebuilt;
      automaton = sg_pair_snapshot_automaton(candidate);
      store_context.automaton = automaton;
      set_full_rebuild_metrics(&metrics, pair_count, sg_automaton_action_count(automaton));
    }
  }
  metrics.pair_records_written =
      full_rebuild ? pair_count : sg_pair_snapshot_changed_count(candidate);

  char snapshot_token[SG_SNAPSHOT_TOKEN_CAPACITY] = {0};
  bool snapshot_stored = false;
  if (status == SG_OK && !make_snapshot_token(snapshot_token)) {
    status = SG_ERR_RESOURCE_BOUND;
  }
  if (status == SG_OK) {
    status = sg_snapshot_cache_insert(snapshot_cache, model, metadata.oracle_epoch, new_generation,
                                      snapshot_token, candidate, &snapshot_stored);
  }
  (void)snapshot_stored;

  size_t db_write_batches = 0U;
  if (status == SG_OK && !apply_base_changes(graph, memory, model, changes, change_count)) {
    status = SG_ERR_INVALID_MODEL;
  }
  if (status == SG_OK) {
    ++db_write_batches;
  }
  if (status == SG_OK && metadata.pair_edges_materialized) {
    status = replace_pair_edges(&store_context, updates, update_count);
    if (status == SG_OK && update_count != 0U) {
      db_write_batches += (update_count / SG_MATERIALIZE_BATCH) +
                          (update_count % SG_MATERIALIZE_BATCH != 0U ? 1U : 0U);
    }
  }
  if (status == SG_OK) {
    status = persist_snapshot_records(&store_context, candidate, full_rebuild, &db_write_batches);
  }
  if (status == SG_OK && !finish_incremental_update(&store_context, snapshot_token)) {
    status = SG_ERR_INVALID_MODEL;
  }
  if (status == SG_OK) {
    ++db_write_batches;
  }
  if (status != SG_OK) {
    set_status_error(result, "incremental maintenance failed", status);
  } else if (!insert_update_result(result, "UPDATED", full_rebuild ? "FULL_REBUILD" : "INCREMENTAL",
                                   new_generation, metadata.oracle_epoch, change_count,
                                   update_count, &metrics, db_write_batches, fallback_rebuild,
                                   elapsed_us(start), memory)) {
    set_error(result, "failed to create incremental update result");
  }
  free(seed_pairs);
  free(updates);
  free(changes);
  sg_pair_snapshot_release(candidate);
  sg_pair_snapshot_release(base_snapshot);
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

static bool add_optional_int(struct mgp_proc *procedure, const char *name, int64_t default_value,
                             struct mgp_memory *memory, struct mgp_type *type) {
  struct mgp_value *value = NULL;
  if (!mg_ok(mgp_value_make_int(default_value, memory, &value)) || value == NULL) {
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
         add_optional_bool(procedure, "incremental", false, memory, bool_type) &&
         add_result(procedure, "status", string_type) &&
         add_result(procedure, "generation", int_type) &&
         add_result(procedure, "oracle_epoch", int_type) &&
         add_result(procedure, "states", int_type) && add_result(procedure, "actions", int_type) &&
         add_result(procedure, "outputs", int_type) &&
         add_result(procedure, "transitions", int_type) &&
         add_result(procedure, "pairs", int_type) &&
         add_result(procedure, "pair_edges", int_type) &&
         add_result(procedure, "mergeable_pairs", int_type) &&
         add_result(procedure, "resolvable_pairs", int_type) &&
         add_result(procedure, "materialized_pair_edges", bool_type) &&
         add_result(procedure, "incremental_enabled", bool_type);
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
         add_result(procedure, "planning_time_us", int_type) &&
         add_result(procedure, "oracle_source", string_type) &&
         add_result(procedure, "cache_state", string_type) &&
         add_result(procedure, "oracle_builds", int_type) &&
         add_result(procedure, "oracle_rows_loaded", int_type) &&
         add_result(procedure, "oracle_load_batches", int_type) &&
         add_result(procedure, "oracle_cache_hits", int_type) &&
         add_result(procedure, "snapshot_record_reads", int_type) &&
         add_result(procedure, "oracle_time_us", int_type) &&
         add_result(procedure, "snapshot_hydration_time_us", int_type) &&
         add_result(procedure, "total_compute_time_us", int_type);
}

static bool register_plan_sync(struct mgp_module *module, const char *name, mgp_proc_cb callback,
                               struct mgp_type *string_type, struct mgp_type *int_type,
                               struct mgp_type *list_type) {
  struct mgp_proc *procedure = NULL;
  if (!mg_ok(mgp_module_add_read_procedure(module, name, callback, &procedure)) ||
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

static bool register_plan_disambiguate(struct mgp_module *module, const char *name,
                                       mgp_proc_cb callback, struct mgp_type *string_type,
                                       struct mgp_type *bool_type, struct mgp_type *int_type,
                                       struct mgp_type *list_type) {
  struct mgp_proc *procedure = NULL;
  if (!mg_ok(mgp_module_add_read_procedure(module, name, callback, &procedure)) ||
      procedure == NULL) {
    return false;
  }
  const bool restricted = strcmp(name, "plan_disambiguate_allowed") == 0;
  return add_required(procedure, "model", string_type) &&
         add_required(procedure, "hypotheses", list_type) &&
         add_required(procedure, "bound", int_type) &&
         (!restricted || add_required(procedure, "actions", list_type)) &&
         add_required(procedure, "budget", int_type) &&
         add_plan_common_results(procedure, string_type, int_type, list_type) &&
         add_result(procedure, "best_support_size", int_type) &&
         add_result(procedure, "worst_support_size", int_type) &&
         add_result(procedure, "branch_count", int_type) &&
         add_result(procedure, "homing", bool_type);
}


static void plan_disambiguate_allowed_cb(struct mgp_list *arguments, struct mgp_graph *graph,
                                         struct mgp_result *result, struct mgp_memory *memory) {
  plan_disambiguate_impl(arguments, graph, result, memory, ORACLE_SOURCE_PERSISTED, true);
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

static bool register_update_cells(struct mgp_module *module, struct mgp_memory *memory,
                                  struct mgp_type *string_type, struct mgp_type *bool_type,
                                  struct mgp_type *int_type, struct mgp_type *map_list_type) {
  struct mgp_proc *procedure = NULL;
  if (!mg_ok(mgp_module_add_write_procedure(module, "update_cells", update_cells_cb, &procedure)) ||
      procedure == NULL) {
    return false;
  }
  return add_required(procedure, "model", string_type) &&
         add_required(procedure, "changes", map_list_type) &&
         add_optional_int(procedure, "repair_budget", 0, memory, int_type) &&
         add_result(procedure, "status", string_type) &&
         add_result(procedure, "maintenance_mode", string_type) &&
         add_result(procedure, "generation", int_type) &&
         add_result(procedure, "oracle_epoch", int_type) &&
         add_result(procedure, "changed_cells", int_type) &&
         add_result(procedure, "direct_pair_edges", int_type) &&
         add_result(procedure, "pair_records_touched", int_type) &&
         add_result(procedure, "pair_records_examined", int_type) &&
         add_result(procedure, "pair_records_written", int_type) &&
         add_result(procedure, "pair_edges_examined", int_type) &&
         add_result(procedure, "db_write_batches", int_type) &&
         add_result(procedure, "merge_pairs_changed", int_type) &&
         add_result(procedure, "merge_pairs_invalidated", int_type) &&
         add_result(procedure, "resolution_pairs_changed", int_type) &&
         add_result(procedure, "resolution_pairs_invalidated", int_type) &&
         add_result(procedure, "fallback_rebuild", bool_type) &&
         add_result(procedure, "maintenance_time_us", int_type);
}

int mgp_init_module(struct mgp_module *module, struct mgp_memory *memory) {
  struct mgp_type *string_type = NULL;
  struct mgp_type *bool_type = NULL;
  struct mgp_type *int_type = NULL;
  struct mgp_type *list_type = NULL;
  struct mgp_type *map_type = NULL;
  struct mgp_type *map_list_type = NULL;
  size_t cache_limit = 0U;
  atomic_store_explicit(&snapshot_counter, UINT64_C(1), memory_order_relaxed);
  if (snapshot_cache != NULL || !initialize_snapshot_incarnation() ||
      !snapshot_cache_limit(&cache_limit) ||
      sg_snapshot_cache_create(cache_limit, &snapshot_cache) != SG_OK) {
    return 1;
  }
  if (!mg_ok(mgp_type_string(&string_type)) || !mg_ok(mgp_type_bool(&bool_type)) ||
      !mg_ok(mgp_type_int(&int_type)) || !mg_ok(mgp_type_list(string_type, &list_type)) ||
      !mg_ok(mgp_type_map(&map_type)) || !mg_ok(mgp_type_list(map_type, &map_list_type))) {
    sg_snapshot_cache_free(snapshot_cache);
    snapshot_cache = NULL;
    return 1;
  }
  if (!register_prepare(module, memory, string_type, bool_type, int_type) ||
      !register_plan_sync(module, "plan_sync", plan_sync_cb, string_type, int_type, list_type) ||
      !register_plan_sync(module, "plan_sync_uncached", plan_sync_uncached_cb, string_type,
                          int_type, list_type) ||
      !register_plan_disambiguate(module, "plan_disambiguate", plan_disambiguate_cb, string_type,
                                  bool_type, int_type, list_type) ||
      !register_plan_disambiguate(module, "plan_disambiguate_uncached",
                                  plan_disambiguate_uncached_cb, string_type, bool_type, int_type,
                                  list_type) ||
      !register_plan_disambiguate(module, "plan_disambiguate_allowed",
                                  plan_disambiguate_allowed_cb, string_type, bool_type, int_type,
                                  list_type) ||
      !register_plan_sync_allowed(module, string_type, int_type, list_type) ||
      !register_explain(module, string_type, int_type, list_type) ||
      !register_validate(module, memory, string_type, bool_type, int_type, list_type) ||
      !register_update_cells(module, memory, string_type, bool_type, int_type, map_list_type) ||
      !register_mark_dirty(module, string_type, int_type)) {
    sg_snapshot_cache_free(snapshot_cache);
    snapshot_cache = NULL;
    return 1;
  }
  return 0;
}

int mgp_shutdown_module(void) {
  sg_snapshot_cache_free(snapshot_cache);
  snapshot_cache = NULL;
  return 0;
}
