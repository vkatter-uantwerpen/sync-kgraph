#!/usr/bin/env sh
set -eu

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <builddir> <target-name> <outdir>" >&2
  exit 2
fi

builddir="$1"
target="$2"
outdir="$3"
stagedir="$outdir/sync-kgraph-$target"

rm -rf "$stagedir"
mkdir -p \
  "$stagedir/bin" \
  "$stagedir/lib" \
  "$stagedir/include/sync_kgraph" \
  "$stagedir/cypher" \
  "$stagedir/examples" \
  "$stagedir/views"

cp "$builddir/sync-kgraph-cli" "$stagedir/bin/"
cp "$builddir/libsync_kgraph.a" "$stagedir/lib/"
if [ -f "$builddir/sync.so" ]; then
  cp "$builddir/sync.so" "$stagedir/lib/"
elif [ -f "$builddir/sync.dylib" ]; then
  cp "$builddir/sync.dylib" "$stagedir/lib/"
fi

cp README.md HACKING.md LICENSE "$stagedir/"
cp include/sync_kgraph/sync.h "$stagedir/include/sync_kgraph/"
cp cypher/*.cypher "$stagedir/cypher/"
cp -R examples/warehouse "$stagedir/examples/"
cp views/sync_automata.gss "$stagedir/views/"

tar -C "$outdir" -czf "$outdir/sync-kgraph-$target.tar.gz" "sync-kgraph-$target"
if command -v shasum >/dev/null 2>&1; then
  shasum -a 256 "$outdir/sync-kgraph-$target.tar.gz" >"$outdir/sync-kgraph-$target.tar.gz.sha256"
else
  sha256sum "$outdir/sync-kgraph-$target.tar.gz" >"$outdir/sync-kgraph-$target.tar.gz.sha256"
fi
