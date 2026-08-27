# Warehouse example

Run the numbered Cypher files in order. The model has five orientation-aware
states, four controller actions, and four abstract sensor outputs. Preparation
validates all 20 transition cells and all 20 observation cells before writing
the pair oracle.

The ambiguous bay hypotheses synchronize at `dock:north` under
`["to_corridor", "go_west"]`. The first action alone emits different landmark
outputs, so `["to_corridor"]` is also a homing disambiguation word.

Files 00 through 05 demonstrate a visual snapshot oracle. File 06 reprepares
the same model in incremental mode and atomically changes one observation cell
while preserving the oracle epoch.
