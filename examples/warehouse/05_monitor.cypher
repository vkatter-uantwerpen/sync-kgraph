CALL sync.validate_update(
  "warehouse", 1,
  ["west_bay:east", "east_bay:west"],
  ["to_corridor", "go_west"],
  1,
  ["corridor_w:east", "corridor_e:west"],
  true
)
YIELD status, decision, reason, expected_hypotheses, unexpected_hypotheses, generation
RETURN status, decision, reason, expected_hypotheses, unexpected_hypotheses, generation;

// Expected decision: "CONTINUE"
