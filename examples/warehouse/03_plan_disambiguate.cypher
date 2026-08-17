CALL sync.plan_disambiguate(
  "warehouse",
  ["west_bay:east", "east_bay:west"],
  1,
  64
)
YIELD status, outcome, method, word, length, best_support_size,
      worst_support_size, branch_count, homing, generation
RETURN status, outcome, method, word, length, best_support_size,
       worst_support_size, branch_count, homing, generation;

// Expected row:
// "OK", "PLAN", "PAIR_RESOLUTION", ["to_corridor"], 1,
// 1, 1, 2, true, 1
