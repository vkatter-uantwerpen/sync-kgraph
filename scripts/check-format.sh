#!/usr/bin/env sh
set -eu

clang-format --dry-run --Werror \
  include/sync_kgraph/sync.h \
  src/dynamic.c \
  src/dynamic.h \
  src/oracle.c \
  src/planner.c \
  src/sync.c \
  src/sync_internal.h \
  src/sync_cli.c \
  src/memgraph/sync_module.c \
  tests/test_core.c
