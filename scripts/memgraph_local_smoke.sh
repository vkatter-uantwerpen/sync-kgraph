#!/usr/bin/env sh
set -eu

builddir="${1:-build-memgraph-local}"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
case "$builddir" in
/*) module="$builddir/sync.so" ;;
*) module="$root/$builddir/sync.so" ;;
esac
memgraph="${MEMGRAPH_BINARY:-/usr/lib/memgraph/memgraph}"
port="${MEMGRAPH_TEST_PORT:-$((17687 + ($$ % 10000)))}"
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/sync-kgraph-memgraph.XXXXXX")
pid=""

cleanup() {
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  rm -rf "$tmpdir"
}
trap cleanup EXIT HUP INT TERM

dump_logs() {
  if [ -f "$tmpdir/stderr.log" ]; then
    cat "$tmpdir/stderr.log" >&2
  fi
  if [ -f "$tmpdir/memgraph.log" ]; then
    cat "$tmpdir/memgraph.log" >&2
  fi
}

if [ ! -f "$module" ]; then
  echo "missing Memgraph module: $module" >&2
  exit 2
fi
if [ ! -x "$memgraph" ]; then
  echo "missing Memgraph executable: $memgraph" >&2
  exit 2
fi
if ! command -v mgconsole >/dev/null 2>&1; then
  echo "mgconsole is required for the local integration test" >&2
  exit 2
fi

mkdir -p "$tmpdir/data" "$tmpdir/modules" "$tmpdir/query-logs"
cp "$module" "$tmpdir/modules/sync.so"

"$memgraph" \
  --bolt-address=127.0.0.1 \
  --bolt-port="$port" \
  --data-directory="$tmpdir/data" \
  --data-recovery-on-startup=false \
  --log-file="$tmpdir/memgraph.log" \
  --metrics-address=127.0.0.1 \
  --metrics-port="$((port + 1))" \
  --monitoring-address=127.0.0.1 \
  --monitoring-port="$((port + 2))" \
  --query-log-directory="$tmpdir/query-logs" \
  --query-modules-directory="$tmpdir/modules" \
  --storage-snapshot-interval-sec=0 \
  --storage-snapshot-on-exit=false \
  --storage-wal-enabled=false \
  --telemetry-enabled=false >"$tmpdir/stderr.log" 2>&1 &
pid=$!

ready=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  if ! kill -0 "$pid" 2>/dev/null; then
    dump_logs
    echo "isolated Memgraph process exited before becoming ready" >&2
    exit 1
  fi
  if printf 'RETURN 1;\n' | mgconsole \
    --host=127.0.0.1 --port="$port" --no_history >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 1
done

if [ "$ready" -ne 1 ]; then
  dump_logs
  echo "isolated Memgraph process did not become ready" >&2
  exit 1
fi

MEMGRAPH_HOST=127.0.0.1 MEMGRAPH_PORT="$port" \
  sh "$root/scripts/memgraph_integration.sh" local

echo "Local smoke used isolated temporary data and left the installed database untouched"
