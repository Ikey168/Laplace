#!/usr/bin/env bash
# Dynamically linked programs for the #254 gate, with what they load, under
# build/ext/dyn/. Each manifest line is "host-path:path-in-the-machine".
#
#   musl.manifest   hello (user/linux/hello.c) built by musl-gcc without
#                   -static, and musl's loader /lib/ld-musl-x86_64.so.1
#   glibc.manifest  the distribution's lua5.4 (installed, else fetched with
#                   apt-get download), its loader, and the host's copies of
#                   the libraries ldd says it needs
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=build/ext/dyn
MUSL_CC=${MUSL_CC:-musl-gcc}
mkdir -p "$OUT"

if [ ! -s "$OUT/musl.manifest" ]; then
    "$MUSL_CC" -march=x86-64 -mtune=generic -O2 -g -o "$OUT/hello_dyn" user/linux/hello.c
    interp=$(readelf -l "$OUT/hello_dyn" | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p')
    {
        echo "$OUT/hello_dyn:/bin/hello_dyn"
        echo "$(readlink -f "$interp"):$interp"
    } > "$OUT/musl.manifest"
fi

if [ ! -s "$OUT/glibc.manifest" ]; then
    if [ -x /usr/bin/lua5.4 ]; then
        bin=/usr/bin/lua5.4                              # installed (CI: apt-get install lua5.4)
    else
        mkdir -p "$OUT/deb/x"
        (cd "$OUT/deb" && apt-get download lua5.4 >/dev/null)
        dpkg-deb -x "$OUT"/deb/lua5.4_*.deb "$OUT/deb/x"
        bin="$OUT/deb/x/usr/bin/lua5.4"
    fi
    interp=$(readelf -l "$bin" | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p')
    {
        echo "$bin:/usr/bin/lua5.4"
        echo "$(readlink -f "$interp"):$interp"
        ldd "$bin" | sed -n 's/^\s*\(\S*\) => \(\/\S*\) .*/\2/p' | while read -r lib; do
            echo "$(readlink -f "$lib"):$lib"
        done
    } > "$OUT/glibc.manifest.tmp"
    mv "$OUT/glibc.manifest.tmp" "$OUT/glibc.manifest"
fi
echo "$OUT: $(wc -l < "$OUT/musl.manifest") musl files, $(wc -l < "$OUT/glibc.manifest") glibc files"
