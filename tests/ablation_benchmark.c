#include "snapshot.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SG_ABLATION_ACTION_COUNT 3U
#define SG_ABLATION_REPETITIONS 5U

static const double SG_ABLATION_REQUIRED_SPEEDUP = 1.5;

typedef struct {
  uint64_t incremental_ns;
  uint64_t full_rebuild_ns;
  size_t records_examined;
  size_t records_written;
  size_t pair_count;
} ablation_result;

static void fail(const char *message) {
  (void)fprintf(stderr, "ablation benchmark failed: %s\n", message);
  exit(EXIT_FAILURE);
}

static uint64_t monotonic_ns(void) {
  struct timespec value = {0};
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
    fail("clock_gettime");
  }
  return ((uint64_t)value.tv_sec * UINT64_C(1000000000)) + (uint64_t)value.tv_nsec;
}

static uint64_t elapsed_ns(uint64_t start) {
  const uint64_t end = monotonic_ns();
  if (end < start) {
    fail("non-monotonic clock");
  }
  return end - start;
}

static int compare_u64(const void *left, const void *right) {
  const uint64_t first = *(const uint64_t *)left;
  const uint64_t second = *(const uint64_t *)right;
  if (first == second) {
    return 0;
  }
  return first < second ? -1 : 1;
}

static sg_automaton *build_fixture(size_t state_count) {
  sg_automaton_builder *builder = NULL;
  if (sg_automaton_builder_init(&builder) != SG_OK) {
    fail("builder initialization");
  }
  char key[64] = {0};
  for (size_t state = 0U; state < state_count; ++state) {
    (void)snprintf(key, sizeof(key), "s%zu", state);
    if (sg_automaton_builder_add_state(builder, key) != SG_OK ||
        sg_automaton_builder_add_output(builder, key) != SG_OK) {
      fail("state/output insertion");
    }
  }
  for (size_t action = 0U; action < SG_ABLATION_ACTION_COUNT; ++action) {
    (void)snprintf(key, sizeof(key), "a%zu", action);
    if (sg_automaton_builder_add_action(builder, key) != SG_OK) {
      fail("action insertion");
    }
  }
  char source[64] = {0};
  char action_key[64] = {0};
  char target[64] = {0};
  for (size_t state = 0U; state < state_count; ++state) {
    (void)snprintf(source, sizeof(source), "s%zu", state);
    for (size_t action = 0U; action < SG_ABLATION_ACTION_COUNT; ++action) {
      const size_t target_state = action == 0U ? 0U : (state + ((action * 6U) - 5U)) % state_count;
      (void)snprintf(action_key, sizeof(action_key), "a%zu", action);
      (void)snprintf(target, sizeof(target), "s%zu", target_state);
      if (sg_automaton_builder_add_transition(builder, source, action_key, target) != SG_OK ||
          sg_automaton_builder_add_observation(builder, source, action_key, source) != SG_OK) {
        fail("cell insertion");
      }
    }
  }
  sg_automaton *automaton = NULL;
  if (sg_automaton_builder_build(builder, UINT64_C(1), &automaton) != SG_OK) {
    fail("automaton construction");
  }
  sg_automaton_builder_free(builder);
  return automaton;
}

static sg_pair_snapshot *make_updated_snapshot(const sg_pair_snapshot *base, size_t state_count,
                                               bool incremental, sg_pair_repair_metrics *metrics) {
  sg_pair_snapshot *candidate = NULL;
  if (sg_pair_snapshot_clone(base, UINT64_C(2), &candidate) != SG_OK) {
    fail("snapshot clone");
  }
  bool changed = false;
  if (sg_pair_snapshot_set_cell(candidate, 0U, 1U, 2U, 0U, &changed) != SG_OK || !changed) {
    fail("snapshot mutation");
  }
  if (!incremental) {
    sg_pair_snapshot *rebuilt = NULL;
    if (sg_pair_snapshot_build(sg_pair_snapshot_automaton(candidate), &rebuilt) != SG_OK) {
      fail("full snapshot rebuild");
    }
    sg_pair_snapshot_release(candidate);
    return rebuilt;
  }
  size_t *seeds = calloc(state_count, sizeof(*seeds));
  if (seeds == NULL) {
    fail("seed allocation");
  }
  for (size_t state = 0U; state < state_count; ++state) {
    seeds[state] = state;
  }
  const sg_status status = sg_pair_snapshot_repair(candidate, seeds, state_count,
                                                   sg_pair_snapshot_pair_count(candidate), metrics);
  free(seeds);
  if (status != SG_OK) {
    fail("incremental snapshot repair");
  }
  return candidate;
}

static void verify_equal(const sg_pair_snapshot *incremental, const sg_pair_snapshot *rebuilt) {
  const size_t pair_count = sg_pair_snapshot_pair_count(incremental);
  if (pair_count != sg_pair_snapshot_pair_count(rebuilt)) {
    fail("pair count mismatch");
  }
  for (size_t pair = 0U; pair < pair_count; ++pair) {
    sg_pair_record first = {0};
    sg_pair_record second = {0};
    if (sg_pair_snapshot_record(incremental, pair, &first) != SG_OK ||
        sg_pair_snapshot_record(rebuilt, pair, &second) != SG_OK ||
        first.mergeable != second.mergeable || first.merge_distance != second.merge_distance ||
        first.merge_action != second.merge_action ||
        first.merge_next_pair != second.merge_next_pair ||
        first.merge_support_count != second.merge_support_count ||
        first.resolvable != second.resolvable ||
        first.resolution_distance != second.resolution_distance ||
        first.resolution_action != second.resolution_action ||
        first.resolution_next_pair != second.resolution_next_pair ||
        first.resolution_support_count != second.resolution_support_count) {
      fail("incremental/full record mismatch");
    }
  }
}

static ablation_result run_size(size_t state_count) {
  sg_automaton *automaton = build_fixture(state_count);
  sg_pair_snapshot *base = NULL;
  if (sg_pair_snapshot_build(automaton, &base) != SG_OK) {
    fail("base snapshot build");
  }
  sg_automaton_free(automaton);

  sg_pair_repair_metrics verification_metrics = {0};
  sg_pair_snapshot *incremental =
      make_updated_snapshot(base, state_count, true, &verification_metrics);
  sg_pair_repair_metrics unused_metrics = {0};
  sg_pair_snapshot *rebuilt = make_updated_snapshot(base, state_count, false, &unused_metrics);
  verify_equal(incremental, rebuilt);
  const size_t changed_count = sg_pair_snapshot_changed_count(incremental);
  const size_t pair_count = sg_pair_snapshot_pair_count(incremental);
  if (changed_count == 0U || changed_count >= pair_count ||
      verification_metrics.pair_records_examined >= pair_count) {
    fail("repair was not output-sensitive");
  }
  sg_pair_snapshot_release(incremental);
  sg_pair_snapshot_release(rebuilt);

  uint64_t incremental_times[SG_ABLATION_REPETITIONS] = {0};
  uint64_t rebuild_times[SG_ABLATION_REPETITIONS] = {0};
  for (size_t repetition = 0U; repetition < SG_ABLATION_REPETITIONS; ++repetition) {
    sg_pair_repair_metrics metrics = {0};
    uint64_t start = monotonic_ns();
    incremental = make_updated_snapshot(base, state_count, true, &metrics);
    incremental_times[repetition] = elapsed_ns(start);
    sg_pair_snapshot_release(incremental);

    start = monotonic_ns();
    rebuilt = make_updated_snapshot(base, state_count, false, &unused_metrics);
    rebuild_times[repetition] = elapsed_ns(start);
    sg_pair_snapshot_release(rebuilt);
  }
  sg_pair_snapshot_release(base);
  qsort(incremental_times, SG_ABLATION_REPETITIONS, sizeof(*incremental_times), compare_u64);
  qsort(rebuild_times, SG_ABLATION_REPETITIONS, sizeof(*rebuild_times), compare_u64);
  const size_t median = SG_ABLATION_REPETITIONS / 2U;
  return (ablation_result){
      .incremental_ns = incremental_times[median],
      .full_rebuild_ns = rebuild_times[median],
      .records_examined = verification_metrics.pair_records_examined,
      .records_written = changed_count,
      .pair_count = pair_count,
  };
}

int main(int argc, char **argv) {
  static const size_t state_counts[] = {198U, 512U, 1000U};
  if (argc > 2) {
    (void)fprintf(stderr, "usage: %s [output.csv]\n", argv[0]);
    return EXIT_FAILURE;
  }
  FILE *output = stdout;
  FILE *output_file = NULL;
  if (argc == 2) {
    output_file = fopen(argv[1], "w");
    if (output_file == NULL) {
      fail("output file");
    }
    output = output_file;
  }
  (void)fprintf(output, "states,pairs,records_examined,records_written,"
                        "median_incremental_ns,median_full_rebuild_ns,speedup\n");
  for (size_t index = 0U; index < sizeof(state_counts) / sizeof(state_counts[0]); ++index) {
    const size_t states = state_counts[index];
    const ablation_result result = run_size(states);
    const double speedup = (double)result.full_rebuild_ns / (double)result.incremental_ns;
    (void)fprintf(output, "%zu,%zu,%zu,%zu,%" PRIu64 ",%" PRIu64 ",%.3f\n", states,
                  result.pair_count, result.records_examined, result.records_written,
                  result.incremental_ns, result.full_rebuild_ns, speedup);
    (void)fflush(output);
    if (speedup < SG_ABLATION_REQUIRED_SPEEDUP) {
      fail("incremental speedup is below 1.5x");
    }
  }
  if (output_file != NULL && fclose(output_file) != 0) {
    fail("output close");
  }
  return EXIT_SUCCESS;
}
