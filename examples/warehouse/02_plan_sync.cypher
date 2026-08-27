CALL sync.plan_sync(
  "warehouse",
  ["west_bay:east", "east_bay:west"],
  64
)
YIELD status, outcome, method, word, length, final_state_key,
      final_support_size, generation, cache_state
RETURN status, outcome, method, word, length, final_state_key,
       final_support_size, generation, cache_state;

// Expected row:
// "OK", "PLAN", "PAIR_MERGE", ["to_corridor", "go_west"], 2,
// "dock:north", 1, 1, "HOT"
