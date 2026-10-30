#!/usr/bin/env bash
# The release tarball: build/dist/laplace-VERSION-x86_64.tar.gz and its
# SHA256SUMS.
#
#   laplace-VERSION-x86_64/
#     boot/laplace.elf      the kernel (Multiboot; QEMU -kernel)
#     boot/laplace.iso      a GRUB rescue ISO for BIOS machines
#     seed/rootfs.cpio      the seed image the gates use (rootfs/)
#     seed/demo.cpio        the same, plus /bin/lua (Lua 5.4.6), /bin/sqlite3
#                           (SQLite 3.44.2), both static musl, and /bin/hello_dyn
#                           with musl's loader (a dynamically linked program)
#     programs/             the user programs, with symbols for gdb
#     tools/laplace-run     boot it in QEMU
#     tools/laplace-mcp     the stdio MCP bridge
#     README.md LICENSE CHANGELOG.md
#
# Needs what `make` and `make iso` need, plus git and network access for the
# Lua and SQLite sources (scripts/build/lua.sh, sqlite.sh).
set -euo pipefail
cd "$(dirname "$0")/../.."

VERSION=$(cat VERSION)
NAME=laplace-$VERSION-x86_64
DIST=build/dist
STAGE=$DIST/$NAME
MUSL_CC=${MUSL_CC:-musl-gcc}

make -s all
make -s iso
bash scripts/build/lua.sh
bash scripts/build/sqlite.sh

# A dynamically linked musl program and the loader it names.
mkdir -p build/ext/dist
"$MUSL_CC" -march=x86-64 -mtune=generic -O2 -g -o build/ext/dist/hello_dyn user/linux/hello.c
interp=$(readelf -l build/ext/dist/hello_dyn | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p')

rm -rf "$STAGE" "$DIST/$NAME.tar.gz"
mkdir -p "$STAGE/boot" "$STAGE/seed" "$STAGE/programs" "$STAGE/tools"
cp build/laplace.elf build/laplace.iso "$STAGE/boot/"
cp build/rootfs.cpio "$STAGE/seed/"
python3 tools/mkcpio.py rootfs "$STAGE/seed/demo.cpio" \
    --add build/ext/lua:/bin/lua \
    --add build/ext/sqlite3:/bin/sqlite3 \
    --add build/ext/dist/hello_dyn:/bin/hello_dyn \
    --add "$(readlink -f "$interp"):$interp"
cp build/user/*.elf "$STAGE/programs/"
cp build/ext/lua "$STAGE/programs/lua.elf"
cp build/ext/sqlite3 "$STAGE/programs/sqlite3.elf"
cp build/ext/dist/hello_dyn "$STAGE/programs/hello_dyn.elf"
cp tools/laplace-run tools/laplace-mcp "$STAGE/tools/"
cp README.md LICENSE CHANGELOG.md "$STAGE/"

# Reproducible archive: sorted names, fixed owner and time.
mtime=$(git log -1 --format=%ct 2>/dev/null || echo 0)
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$mtime" \
    -C "$DIST" -czf "$DIST/$NAME.tar.gz" "$NAME"
cp build/laplace.iso "$DIST/$NAME.iso"
(cd "$DIST" && sha256sum "$NAME.tar.gz" "$NAME.iso" > SHA256SUMS)
echo "$DIST/$NAME.tar.gz: $(stat -c %s "$DIST/$NAME.tar.gz") bytes"
cat "$DIST/SHA256SUMS"
