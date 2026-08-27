#!/usr/bin/env sh
set -eu

clang-format --dry-run --Werror \
  include/sync_kgraph/sync.h \
  src/dynamic.c \
  src/dynamic.h \
  src/oracle.c \
  src/planner.c \
  src/snapshot.c \
  src/snapshot.h \
  src/snapshot_cache.c \
  src/snapshot_cache.h \
  src/sync.c \
  src/sync_internal.h \
  src/sync_cli.c \
  src/memgraph/sync_module.c \
  tests/ablation_benchmark.c \
  tests/test_core.c
