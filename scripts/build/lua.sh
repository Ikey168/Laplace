#!/usr/bin/env bash
# Build Lua 5.4.6, unmodified, as a static musl binary: build/ext/lua (#247, #256).
#
# The source is the official Lua repository on GitHub, at the v5.4.6 tag,
# checked against its commit. Built once; later runs reuse the binary.
set -euo pipefail
cd "$(dirname "$0")/../.."

TAG=v5.4.6
COMMIT=6443185167c77adcc8552a3fee7edab7895db1a9
OUT=build/ext/lua
SRC=build/ext/lua-src
MUSL_CC=${MUSL_CC:-musl-gcc}

if [ -x "$OUT" ]; then echo "$OUT (cached)"; exit 0; fi
mkdir -p build/ext
if [ ! -d "$SRC" ]; then
    git -c advice.detachedHead=false clone -q --depth 1 --branch "$TAG" https://github.com/lua/lua "$SRC"
fi
have=$(git -C "$SRC" rev-parse HEAD)
if [ "$have" != "$COMMIT" ]; then
    echo "lua: $TAG is $have, expected $COMMIT" >&2
    exit 1
fi
# onelua.c is the whole interpreter in one translation unit.
"$MUSL_CC" -march=x86-64 -mtune=generic -static -O2 -std=gnu99 -DLUA_USE_POSIX -o "$OUT" "$SRC/onelua.c" -lm
echo "$OUT: Lua ${TAG#v} ($COMMIT), $(stat -c %s "$OUT") bytes"
