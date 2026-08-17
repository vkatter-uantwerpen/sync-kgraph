#!/usr/bin/env sh
set -eu

builddir="${1:-build-coverage}"

CC="${CC:-clang}" meson setup "$builddir" --wipe -Db_coverage=true -Dmemgraph=disabled
meson test -C "$builddir" --print-errorlogs
gcovr \
  --root . \
  --object-directory "$builddir" \
  --gcov-executable "llvm-cov gcov" \
  --filter 'src/(oracle|planner|sync)\.c' \
  --exclude 'tests/.*' \
  --fail-under-line 75 \
  --print-summary \
  "$builddir"
