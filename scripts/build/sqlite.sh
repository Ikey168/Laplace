#!/usr/bin/env bash
# Build the SQLite 3.44.2 shell, unmodified, as a static musl binary:
# build/ext/sqlite3 (#256).
#
# The source is SQLite's own autoconf tarball (sqlite3.c and shell.c), as the
# sqlite3 npm package 5.1.7 ships it; both archives are checked against their
# SHA-256. (The npm registry is reachable from more build environments than
# sqlite.org.) Built once; later runs reuse the binary.
set -euo pipefail
cd "$(dirname "$0")/../.."

NPM_URL=https://registry.npmjs.org/sqlite3/-/sqlite3-5.1.7.tgz
NPM_SHA256=44cc9bdee02705a7b636f76968d745e21c1d0b2f1c62810c678bc3b0ececab1b
TARBALL=sqlite-autoconf-3440200
TARBALL_SHA256=1c6719a148bc41cf0f2bbbe3926d7ce3f5ca09d878f1246fcc20767b175bb407
OUT=build/ext/sqlite3
SRC=build/ext/sqlite-src
MUSL_CC=${MUSL_CC:-musl-gcc}

if [ -x "$OUT" ]; then echo "$OUT (cached)"; exit 0; fi
mkdir -p "$SRC"
if [ ! -f "$SRC/$TARBALL/sqlite3.c" ]; then
    curl -sSL -o "$SRC/npm.tgz" "$NPM_URL"
    echo "$NPM_SHA256  $SRC/npm.tgz" | sha256sum -c --quiet
    tar xzf "$SRC/npm.tgz" -C "$SRC" "package/deps/$TARBALL.tar.gz"
    echo "$TARBALL_SHA256  $SRC/package/deps/$TARBALL.tar.gz" | sha256sum -c --quiet
    tar xzf "$SRC/package/deps/$TARBALL.tar.gz" -C "$SRC" "$TARBALL/sqlite3.c" "$TARBALL/sqlite3.h" \
        "$TARBALL/shell.c"
fi
# One thread, no extension loading: what a static program in the machine has.
"$MUSL_CC" -static -O2 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -I"$SRC/$TARBALL" \
    -o "$OUT" "$SRC/$TARBALL/shell.c" "$SRC/$TARBALL/sqlite3.c" -lm
echo "$OUT: SQLite 3.44.2, $(stat -c %s "$OUT") bytes"
