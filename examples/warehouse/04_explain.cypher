CALL sync.explain_plan(
  "warehouse",
  1,
  ["west_bay:east", "east_bay:west"],
  ["to_corridor", "go_west"]
)
YIELD step, action, predicted_hypotheses, output_trace, branch_hypotheses, generation
RETURN step, action, predicted_hypotheses, output_trace, branch_hypotheses, generation
ORDER BY step, output_trace;

// Expected: 5 rows. Step 0 has both initial states. Step 1 has one singleton
// branch for west_landmark and one for east_landmark. Step 2 retains those two
// traces, and both branches contain only dock:north.
