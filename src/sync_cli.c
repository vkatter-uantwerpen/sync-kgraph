#include "sync_kgraph/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *source;
  const char *action;
  const char *target;
  const char *output;
} machine_cell;

static sg_status build_warehouse(sg_automaton **automaton) {
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
  sg_status status = sg_automaton_builder_init(&builder);
  for (size_t index = 0U; status == SG_OK && index < sizeof(states) / sizeof(states[0]); ++index) {
    status = sg_automaton_builder_add_state(builder, states[index]);
  }
  for (size_t index = 0U; status == SG_OK && index < sizeof(actions) / sizeof(actions[0]);
       ++index) {
    status = sg_automaton_builder_add_action(builder, actions[index]);
  }
  for (size_t index = 0U; status == SG_OK && index < sizeof(outputs) / sizeof(outputs[0]);
       ++index) {
    status = sg_automaton_builder_add_output(builder, outputs[index]);
  }
  for (size_t index = 0U; status == SG_OK && index < sizeof(cells) / sizeof(cells[0]); ++index) {
    status = sg_automaton_builder_add_transition(builder, cells[index].source, cells[index].action,
                                                 cells[index].target);
    if (status == SG_OK) {
      status = sg_automaton_builder_add_observation(builder, cells[index].source,
                                                    cells[index].action, cells[index].output);
    }
  }
  if (status == SG_OK) {
    status = sg_automaton_builder_build(builder, 1U, automaton);
  }
  sg_automaton_builder_free(builder);
  return status;
}

static void print_word(const sg_automaton *automaton, const sg_word *word) {
  printf("[");
  for (size_t index = 0U; index < word->length; ++index) {
    printf("%s\"%s\"", index == 0U ? "" : ", ",
           sg_automaton_action_key(automaton, word->actions[index]));
  }
  printf("]");
}

static int run_warehouse_example(void) {
  sg_automaton *automaton = NULL;
  sg_status status = build_warehouse(&automaton);
  if (status != SG_OK) {
    fprintf(stderr, "build failed: %s\n", sg_status_name(status));
    return EXIT_FAILURE;
  }
  sg_pair_oracle *oracle = NULL;
  status = sg_pair_oracle_build(automaton, &oracle);
  if (status != SG_OK) {
    fprintf(stderr, "prepare failed: %s\n", sg_status_name(status));
    sg_automaton_free(automaton);
    return EXIT_FAILURE;
  }
  size_t initial[2] = {0U};
  (void)sg_automaton_find_state(automaton, "west_bay:east", &initial[0]);
  (void)sg_automaton_find_state(automaton, "east_bay:west", &initial[1]);

  sg_plan_result sync = {0};
  status = sg_plan_sync(automaton, oracle, initial, 2U, 64U, &sync);
  if (status != SG_OK) {
    fprintf(stderr, "synchronization failed: %s\n", sg_status_name(status));
    sg_pair_oracle_free(oracle);
    sg_automaton_free(automaton);
    return EXIT_FAILURE;
  }
  printf("generation: %llu\n", (unsigned long long)sync.generation);
  printf("sync.outcome: %s\n", sg_plan_outcome_name(sync.outcome));
  printf("sync.method: %s\n", sg_plan_method_name(sync.method));
  printf("sync.word: ");
  print_word(automaton, &sync.word);
  printf("\nsync.final_state: %s\n", sg_automaton_state_key(automaton, sync.final_state));

  const size_t reveal_actions[] = {0U, 1U, 2U, 3U};
  sg_plan_result reveal = {0};
  status =
      sg_plan_disambiguate(automaton, oracle, initial, 2U, 1U, reveal_actions, 4U, 64U, &reveal);
  if (status == SG_OK) {
    printf("reveal.outcome: %s\n", sg_plan_outcome_name(reveal.outcome));
    printf("reveal.method: %s\n", sg_plan_method_name(reveal.method));
    printf("reveal.word: ");
    print_word(automaton, &reveal.word);
    printf("\nreveal.branches: %zu\n", reveal.branch_count);
    printf("reveal.worst_support: %zu\n", reveal.worst_support_size);
  } else {
    fprintf(stderr, "disambiguation failed: %s\n", sg_status_name(status));
  }

  sg_plan_result_free(&reveal);
  sg_plan_result_free(&sync);
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);
  return status == SG_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void print_usage(const char *program) {
  fprintf(stderr, "usage: %s --example warehouse\n", program);
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "--example") == 0 && strcmp(argv[2], "warehouse") == 0) {
    return run_warehouse_example();
  }
  print_usage(argv[0]);
  return 2;
}
