#!/usr/bin/env sh
set -eu

builddir="${1:-build-coverage}"
memgraph_include_dir="${2:-}"

if [ -z "$memgraph_include_dir" ] && [ -f /usr/include/memgraph/mg_procedure.h ]; then
  memgraph_include_dir=/usr/include/memgraph
fi
if [ -z "$memgraph_include_dir" ] ||
   [ ! -f "$memgraph_include_dir/mg_procedure.h" ]; then
  echo "coverage requires a directory containing mg_procedure.h" >&2
  exit 2
fi

CC="${CC:-clang}" meson setup "$builddir" --wipe \
  -Db_coverage=true \
  -Dmemgraph=enabled \
  -Dmemgraph_include_dir="$memgraph_include_dir"
meson compile -C "$builddir"
meson test -C "$builddir" --print-errorlogs
case "$builddir" in
/*) cli="$builddir/sync-kgraph-cli" ;;
*) cli="./$builddir/sync-kgraph-cli" ;;
esac
"$cli" --example warehouse >/dev/null

case "${SYNC_KGRAPH_COVERAGE_RUNNER:-auto}" in
auto)
  if [ -x /usr/lib/memgraph/memgraph ] && command -v mgconsole >/dev/null 2>&1; then
    sh scripts/memgraph_local_smoke.sh "$builddir"
  else
    SYNC_KGRAPH_COVERAGE=1 sh scripts/memgraph_smoke.sh "$builddir"
  fi
  ;;
local)
  sh scripts/memgraph_local_smoke.sh "$builddir"
  ;;
docker)
  SYNC_KGRAPH_COVERAGE=1 sh scripts/memgraph_smoke.sh "$builddir"
  ;;
*)
  echo "SYNC_KGRAPH_COVERAGE_RUNNER must be auto, local, or docker" >&2
  exit 2
  ;;
esac

gcovr \
  --root . \
  --object-directory "$builddir" \
  --gcov-executable "llvm-cov gcov" \
  --filter 'src/.*\.c' \
  --exclude 'tests/.*' \
  --fail-under-line 75 \
  --xml "$builddir/coverage.xml" \
  --html-details "$builddir/coverage.html" \
  --print-summary \
  "$builddir"
