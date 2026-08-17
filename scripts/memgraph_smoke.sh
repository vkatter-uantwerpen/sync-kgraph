#!/usr/bin/env sh
set -eu

builddir="${1:-build-memgraph}"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
case "$builddir" in
/*) module="$builddir/sync.so" ;;
*) module="$root/$builddir/sync.so" ;;
esac
image="${MEMGRAPH_IMAGE:-memgraph/memgraph:3.11.0}"
container="sync-kgraph-smoke-$$"
absolute_builddir=$(dirname -- "$module")
absolute_builddir=$(CDPATH= cd -- "$absolute_builddir" && pwd)

if [ ! -f "$module" ]; then
  echo "missing Memgraph module: $module" >&2
  exit 2
fi

cleanup() {
  docker stop --time 30 "$container" >/dev/null 2>&1 || true
  docker rm -f "$container" >/dev/null 2>&1 || true
}
trap cleanup EXIT HUP INT TERM

if [ "${SYNC_KGRAPH_COVERAGE:-0}" = "1" ]; then
  strip=$(printf '%s\n' "$absolute_builddir" | awk -F/ '{print NF - 1}')
  chmod -R a+rwX "$absolute_builddir"
  docker run \
    -d \
    --name "$container" \
    -e GCOV_PREFIX=/coverage \
    -e GCOV_PREFIX_STRIP="$strip" \
    -v "$absolute_builddir:/coverage" \
    -v "$module:/usr/lib/memgraph/query_modules/sync.so:ro" \
    "$image" \
    --also-log-to-stderr >/dev/null
else
  docker run \
    -d \
    --name "$container" \
    -v "$module:/usr/lib/memgraph/query_modules/sync.so:ro" \
    "$image" \
    --also-log-to-stderr >/dev/null
fi

ready=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  if printf 'RETURN 1;\n' | docker exec -i "$container" mgconsole \
    --host=127.0.0.1 --port=7687 --no_history >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 1
done

if [ "$ready" -ne 1 ]; then
  docker logs "$container" >&2
  echo "Memgraph did not become ready" >&2
  exit 1
fi

sh "$root/scripts/memgraph_integration.sh" docker "$container"
