#!/usr/bin/env bash
# In-QEMU gate for files (#247).
#
#  1. musl_fileprobe (user/linux/fileprobe.c) reads /etc/motd from the seed
#     image (rootfs/, packed by tools/mkcpio.py) and exercises create, write,
#     seek, holes, pread/pwrite, writev, dup, fcntl, ftruncate, file mmap,
#     rename, link, symlink, readlink, readdir, unlink while open, rmdir, and
#     /dev/null and /dev/urandom, with a keyframe every few ticks. Every epoch
#     replays with no divergence; the filesystem is checksummed as its own
#     component.
#  2. Lua 5.4.6, unmodified (scripts/build/lua.sh), runs /scripts/files.lua from
#     the seed image: it reads the seed, writes a file, reads it back, renames
#     and removes it. The recording replays byte-exact.
#  3. A recording on a disk refuses to resume with a different seed image.
set -euo pipefail
cd "$(dirname "$0")/../.."

QEMU=${QEMU:-qemu-system-x86_64}
fail=0
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

check() {  # check <output> <pattern> <description>
    if echo "$1" | grep -qE "$2"; then echo "[ok] $3"; else echo "[FAIL] $3"; fail=1; fi
}

boot() {   # boot <initrd> <append>
    timeout 600 "$QEMU" -m 512M -display none -no-reboot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -kernel build/laplace.elf -initrd "$1" -append "$2 selftest exit" \
        -serial stdio -serial null -serial null 2>&1 | tr -d '\r' || true
}

python3 tools/mkcpio.py rootfs "$W/root.cpio" >/dev/null
out=$(boot "$W/root.cpio" 'run=musl_fileprobe interval=3 keyframes=64')
echo "$out" | grep -E "files:|fileprobe:|selftest: (window|verify|perturbed kernel-state section 'fs')" | sed "s/^/  [fileprobe] /"
check "$out" "fileprobe: /etc/motd: Welcome to Laplace" "a seed file reads from the image"
check "$out" "fileprobe: all checks passed; sum [0-9a-f]{8}" "the file system calls behave"
check "$out" "selftest: window epochs 1\.\.[0-9]+ \(([1-9][0-9]) keyframes\)" "keyframes fall between the file operations, all retained"
check "$out" "selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged" "every epoch replays to the same files"
check "$out" "perturbed kernel-state section 'fs': divergence detected \(epoch [0-9]+ component [0-9]+: fs\)" \
      "a wrong filesystem restore is caught and named"

bash scripts/build/lua.sh
python3 tools/mkcpio.py rootfs "$W/lua.cpio" --add build/ext/lua:/bin/lua >/dev/null
out=$(boot "$W/lua.cpio" 'run="/bin/lua /scripts/files.lua" interval=2 keyframes=64')
echo "$out" | grep -E "lua:|selftest: (window|verify)" | sed "s/^/  [lua] /"
check "$out" "boot: started /bin/lua as pid 1" "Lua starts from the seed image"
check "$out" "lua: motd says: Welcome to Laplace" "Lua reads a file from the seed"
check "$out" "lua: wrote and read back 200 lines, sum 2686700" "Lua writes a file and reads it back"
check "$out" "lua: renamed and removed it; gone" "Lua renames and removes it"
check "$out" "selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged" "the Lua recording replays byte-exact"

# 3. A different seed on resume.
truncate -s 64M "$W/disk.img"
boot_disk() {   # boot_disk <initrd>
    timeout 120 "$QEMU" -m 256M -display none -no-reboot \
        -drive file="$W/disk.img",format=raw,if=ide,index=0 \
        -kernel build/laplace.elf -initrd "$1" -append "run=musl_fileprobe stop_after=3" \
        -serial stdio -serial null -serial null > "$W/boot.txt" 2>&1 &
    local q=$!
    sleep 15
    kill "$q" 2>/dev/null || true
    wait "$q" 2>/dev/null || true
    tr -d '\r' < "$W/boot.txt"
}
out=$(boot_disk "$W/root.cpio")
check "$out" "fileprobe: all checks passed" "the first boot records on the disk"
echo "changed" > "$W/extra"
python3 tools/mkcpio.py rootfs "$W/root2.cpio" --add "$W/extra":/etc/extra >/dev/null
out=$(boot_disk "$W/root2.cpio")
echo "$out" | grep -E "files: the recording|boot: (resumed|the recording)" | sed "s/^/  [resume] /"
check "$out" "files: the recording's seed image differs from this boot's" \
      "a recording does not resume with another seed image"

if [ $fail -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "PASSED: programs read the seed image and write files, and the files replay with the machine"
