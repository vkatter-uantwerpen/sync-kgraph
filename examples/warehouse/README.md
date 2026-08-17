# Warehouse example

Run the numbered Cypher files in order. The model has five orientation-aware
states, four controller actions, and four abstract sensor outputs. Preparation
validates all 20 transition cells and all 20 observation cells before writing
the generation-1 pair oracle.

The ambiguous bay hypotheses synchronize at `dock:north` under
`["to_corridor", "go_west"]`. The first action alone emits different landmark
outputs, so `["to_corridor"]` is also a homing disambiguation word.
