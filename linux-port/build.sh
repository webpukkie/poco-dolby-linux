#!/bin/sh
# Build the Linux port harness: stage dir (Android stubs + deversioned Dolby
# libs) and the probe binary.
#
# Prerequisites (Debian/Ubuntu cross toolchain):
#   sudo apt install gcc-14-aarch64-linux-gnu qemu-user python3
#   plus an aarch64 LLVM libc++ + libunwind for the Dolby libs:
#     either apt install libc++-19-dev-arm64-cross libunwind-19-dev-arm64-cross
#     or build per docs in README.md
#
# Env:
#   CC       cross gcc              (default aarch64-linux-gnu-gcc)
#   SYSROOT  cross sysroot          (default /tmp/crossroot)
#   CXXROOT  aarch64 libc++ prefix  (default /tmp/crossroot-arm64)
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
CC="${CC:-aarch64-linux-gnu-gcc}"
SYS="${SYSROOT:-/tmp/crossroot}"
CXXROOT="${CXXROOT:-/tmp/crossroot-arm64}"

# If the cross toolchain is not on PATH, try common sysroot layouts.
command -v "$CC" >/dev/null 2>&1 || {
    for d in "/tmp/crossroot/usr/bin" "/usr/bin"; do
        [ -x "$d/$(basename "$CC")" ] && PATH="$d:$PATH" && break
    done
}
# the cross binutils need their host libraries
export LD_LIBRARY_PATH="/tmp/crossroot/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

cd "$DIR"
sh mkstage.sh "$DIR/../binaries/arm64" stage "$CC" "$SYS"

$CC --sysroot="$SYS" -O2 -fPIC -shared -o stage/extra_stubs.so extra_stubs.c

$CC --sysroot="$SYS" -O1 -o probe probe.c dap_effect.c apply_packed.c \
    -Wl,-rpath,"$DIR/stage" -L"$DIR/stage" -ldl -lm

$CC --sysroot="$SYS" -O1 -o pipewire/dolbyd pipewire/dolbyd.c dap_effect.c apply_packed.c \
    -Wl,-rpath,"$DIR/stage" -L"$DIR/stage" -ldl -lm
echo "built ./probe, pipewire/dolbyd and ./stage"
