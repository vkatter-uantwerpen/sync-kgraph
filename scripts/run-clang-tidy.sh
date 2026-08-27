#!/usr/bin/env sh
set -eu

builddir="${1:-build-tidy}"
memgraph_include_dir="${2:-}"

if [ -n "$memgraph_include_dir" ]; then
  CC="${CC:-clang}" meson setup "$builddir" --wipe \
    -Dmemgraph=enabled \
    -Dmemgraph_include_dir="$memgraph_include_dir"
  tidy_files="src/dynamic.c src/oracle.c src/planner.c src/sync.c src/sync_cli.c src/memgraph/sync_module.c tests/test_core.c"
else
  CC="${CC:-clang}" meson setup "$builddir" --wipe -Dmemgraph=disabled
  tidy_files="src/dynamic.c src/oracle.c src/planner.c src/sync.c src/sync_cli.c tests/test_core.c"
fi

ninja -C "$builddir"

clang-tidy \
  -p "$builddir" \
  $tidy_files
