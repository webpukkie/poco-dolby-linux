#!/bin/sh
# Build the Linux port harness: stage dir (Android stubs + deversioned Dolby
# libs), the probe binary and the RPC daemon.
#
# Prerequisites:
#   - an aarch64 cross gcc named aarch64-linux-gnu-gcc on PATH
#   - qemu-aarch64 (or qemu-aarch64-static) to run the built binaries
#   - an aarch64 LLVM libc++/libunwind prefix: run ./setup-toolchain.sh once
#
# Env:
#   CC        cross gcc              (default aarch64-linux-gnu-gcc)
#   SYSROOT   cross sysroot          (default: empty = the compiler's own
#                                    sysroot; a sandbox tree at /tmp/crossroot
#                                    is picked up automatically if present)
#   CXXROOT   aarch64 libc++ prefix  (default: /tmp/crossroot-arm64 if it
#                                    exists, else ~/.local/dap-cross)
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
CC="${CC:-aarch64-linux-gnu-gcc}"
SYS="${SYSROOT:-}"
if [ -z "$SYS" ] && [ -d /tmp/crossroot/usr/aarch64-linux-gnu/lib ]; then
    SYS=/tmp/crossroot
fi
CXXROOT="${CXXROOT:-}"
if [ -z "$CXXROOT" ]; then
    if [ -d /tmp/crossroot-arm64/usr/lib/llvm-19 ]; then
        CXXROOT=/tmp/crossroot-arm64
    else
        CXXROOT="$HOME/.local/dap-cross"
    fi
fi
export CXXROOT

# If the cross toolchain is not on PATH, try common locations.
command -v "$CC" >/dev/null 2>&1 || {
    for d in "$SYS/usr/bin" /tmp/crossroot/usr/bin /usr/bin /usr/local/bin; do
        if [ -x "$d/$(basename "$CC")" ]; then PATH="$d:$PATH"; break; fi
    done
}
# an extracted (not installed) cross tree needs its host binutils libraries
if [ -n "$SYS" ] && [ -d "$SYS/usr/lib/x86_64-linux-gnu" ]; then
    export LD_LIBRARY_PATH="$SYS/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

SYSARG=""
[ -n "$SYS" ] && SYSARG="--sysroot=$SYS"

cd "$DIR"
sh mkstage.sh "$DIR/../binaries/arm64" stage "$CC" "$SYS"

$CC $SYSARG -O2 -fPIC -shared -o stage/extra_stubs.so extra_stubs.c

$CC $SYSARG -O1 -o probe probe.c dap_effect.c apply_packed.c \
    -Wl,-rpath,"$DIR/stage" -L"$DIR/stage" -ldl -lm

$CC $SYSARG -O1 -o pipewire/dolbyd pipewire/dolbyd.c dap_effect.c apply_packed.c \
    -Wl,-rpath,"$DIR/stage" -L"$DIR/stage" -ldl -lm
echo "built ./probe, pipewire/dolbyd and ./stage"
