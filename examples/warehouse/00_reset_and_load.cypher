// This reset is scoped to the worked-example model.
MATCH (n)
WHERE n.model = "warehouse"
  AND (n:SyncModel OR n:SyncState OR n:SyncAction OR n:SyncOutput OR n:SyncPair)
DETACH DELETE n;

CREATE (:SyncModel {model: "warehouse", generation: 1, dirty: true});

UNWIND [
  {key: "west_bay:east", id: 0, zone: "west_bay", orientation: "east"},
  {key: "east_bay:west", id: 1, zone: "east_bay", orientation: "west"},
  {key: "corridor_w:east", id: 2, zone: "corridor_w", orientation: "east"},
  {key: "corridor_e:west", id: 3, zone: "corridor_e", orientation: "west"},
  {key: "dock:north", id: 4, zone: "dock", orientation: "north"}
] AS row
CREATE (:SyncState {
  model: "warehouse",
  state_key: row.key,
  state_id: row.id,
  semantic_ref: row.zone,
  orientation: row.orientation
});

UNWIND [
  {key: "to_corridor", id: 0},
  {key: "to_wall", id: 1},
  {key: "go_west", id: 2},
  {key: "go_east", id: 3}
] AS row
CREATE (:SyncAction {model: "warehouse", action_key: row.key, action_id: row.id});

UNWIND [
  {key: "west_landmark", id: 0},
  {key: "east_landmark", id: 1},
  {key: "symmetric", id: 2},
  {key: "dock", id: 3}
] AS row
CREATE (:SyncOutput {model: "warehouse", output_key: row.key, output_id: row.id});

UNWIND [
  {src: "west_bay:east", action: "to_corridor", dst: "corridor_w:east"},
  {src: "west_bay:east", action: "to_wall", dst: "west_bay:east"},
  {src: "west_bay:east", action: "go_west", dst: "west_bay:east"},
  {src: "west_bay:east", action: "go_east", dst: "west_bay:east"},
  {src: "east_bay:west", action: "to_corridor", dst: "corridor_e:west"},
  {src: "east_bay:west", action: "to_wall", dst: "east_bay:west"},
  {src: "east_bay:west", action: "go_west", dst: "east_bay:west"},
  {src: "east_bay:west", action: "go_east", dst: "east_bay:west"},
  {src: "corridor_w:east", action: "to_corridor", dst: "corridor_w:east"},
  {src: "corridor_w:east", action: "to_wall", dst: "west_bay:east"},
  {src: "corridor_w:east", action: "go_west", dst: "dock:north"},
  {src: "corridor_w:east", action: "go_east", dst: "corridor_w:east"},
  {src: "corridor_e:west", action: "to_corridor", dst: "corridor_e:west"},
  {src: "corridor_e:west", action: "to_wall", dst: "east_bay:west"},
  {src: "corridor_e:west", action: "go_west", dst: "dock:north"},
  {src: "corridor_e:west", action: "go_east", dst: "corridor_e:west"},
  {src: "dock:north", action: "to_corridor", dst: "dock:north"},
  {src: "dock:north", action: "to_wall", dst: "dock:north"},
  {src: "dock:north", action: "go_west", dst: "dock:north"},
  {src: "dock:north", action: "go_east", dst: "dock:north"}
] AS row
MATCH (src:SyncState {model: "warehouse", state_key: row.src})
MATCH (dst:SyncState {model: "warehouse", state_key: row.dst})
CREATE (src)-[:SYNC_TRANS {model: "warehouse", action_key: row.action}]->(dst);

UNWIND [
  {src: "west_bay:east", action: "to_corridor", output: "west_landmark"},
  {src: "west_bay:east", action: "to_wall", output: "symmetric"},
  {src: "west_bay:east", action: "go_west", output: "symmetric"},
  {src: "west_bay:east", action: "go_east", output: "symmetric"},
  {src: "east_bay:west", action: "to_corridor", output: "east_landmark"},
  {src: "east_bay:west", action: "to_wall", output: "symmetric"},
  {src: "east_bay:west", action: "go_west", output: "symmetric"},
  {src: "east_bay:west", action: "go_east", output: "symmetric"},
  {src: "corridor_w:east", action: "to_corridor", output: "symmetric"},
  {src: "corridor_w:east", action: "to_wall", output: "west_landmark"},
  {src: "corridor_w:east", action: "go_west", output: "dock"},
  {src: "corridor_w:east", action: "go_east", output: "symmetric"},
  {src: "corridor_e:west", action: "to_corridor", output: "symmetric"},
  {src: "corridor_e:west", action: "to_wall", output: "east_landmark"},
  {src: "corridor_e:west", action: "go_west", output: "dock"},
  {src: "corridor_e:west", action: "go_east", output: "symmetric"},
  {src: "dock:north", action: "to_corridor", output: "dock"},
  {src: "dock:north", action: "to_wall", output: "dock"},
  {src: "dock:north", action: "go_west", output: "dock"},
  {src: "dock:north", action: "go_east", output: "dock"}
] AS row
MATCH (src:SyncState {model: "warehouse", state_key: row.src})
MATCH (output:SyncOutput {model: "warehouse", output_key: row.output})
CREATE (src)-[:SYNC_OBS {model: "warehouse", action_key: row.action}]->(output);
