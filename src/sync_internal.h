#ifndef SYNC_KGRAPH_SYNC_INTERNAL_H
#define SYNC_KGRAPH_SYNC_INTERNAL_H

#include "sync_kgraph/sync.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sg_automaton {
  uint64_t generation;
  size_t state_count;
  size_t action_count;
  size_t output_count;
  char **state_keys;
  char **action_keys;
  char **output_keys;
  size_t *transitions;
  size_t *observations;
};

struct sg_pair_oracle {
  const sg_automaton *automaton;
  size_t pair_count;
  size_t *first;
  size_t *second;
  size_t *next;
  bool *outputs_differ;
  size_t *merge_distance;
  size_t *merge_action;
  size_t *merge_next;
  size_t *resolution_distance;
  size_t *resolution_action;
  size_t *resolution_next;
};

bool sg_size_multiply(size_t first, size_t second, size_t *product);
char *sg_string_duplicate(const char *value);
sg_status sg_word_extend(sg_word *destination, const sg_word *source);
size_t sg_pair_index(size_t state_count, size_t first, size_t second);
uint64_t sg_monotonic_time_us(void);

#endif
