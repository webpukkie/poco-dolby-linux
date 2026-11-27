#!/bin/sh
# Fetch the aarch64 LLVM libc++/libunwind libraries the Dolby libs link
# against and extract them into a plain prefix directory (no root needed).
#
# Usage: ./setup-toolchain.sh [prefix]      (default ~/.local/dap-cross)
#
# After this runs, build with:   CXXROOT=<prefix> ./build.sh
# (build.sh already defaults CXXROOT to ~/.local/dap-cross, so a plain
#  ./build.sh works if you used the default prefix.)
#
# Needs: curl or wget, and dpkg-deb (Debian/Ubuntu) or bsdtar (Arch).
set -e
PREFIX="${1:-$HOME/.local/dap-cross}"
POOL_LLVM="https://deb.debian.org/debian/pool/main/l/llvm-toolchain-19"
POOL_UNWIND="https://deb.debian.org/debian/pool/main/libu/libunwind"
DEBS="
$POOL_LLVM/libc++-19-dev_19.1.7-3+b1_arm64.deb
$POOL_LLVM/libc++1-19_19.1.7-3+b1_arm64.deb
$POOL_LLVM/libc++abi1-19_19.1.7-3+b1_arm64.deb
$POOL_LLVM/libunwind-19-dev_19.1.7-3+b1_arm64.deb
$POOL_LLVM/libunwind-19_19.1.7-3+b1_arm64.deb
$POOL_UNWIND/libunwind8_1.8.1-0.4_arm64.deb
"

fetch() {
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$2" "$1"
    else
        echo "setup-toolchain.sh: need curl or wget" >&2
        exit 1
    fi
}

extract() {
    if command -v dpkg-deb >/dev/null 2>&1; then
        dpkg-deb -x "$1" "$PREFIX"
    elif command -v bsdtar >/dev/null 2>&1; then
        bsdtar -xOf "$1" 'data.tar.*' | bsdtar -xf - -C "$PREFIX"
    else
        echo "setup-toolchain.sh: need dpkg-deb or bsdtar to unpack .deb" >&2
        exit 1
    fi
}

mkdir -p "$PREFIX"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for url in $DEBS; do
    f="$tmp/$(basename "$url")"
    echo "fetching $(basename "$url")"
    fetch "$url" "$f"
    extract "$f"
done

echo
echo "checking expected files under $PREFIX:"
ok=1
for f in \
    usr/lib/aarch64-linux-gnu/libc++.so.1.0 \
    usr/lib/aarch64-linux-gnu/libc++abi.so.1.0 \
    usr/lib/aarch64-linux-gnu/libunwind.so.8.1.0 \
    usr/lib/llvm-19/lib/libunwind.so.1.0
do
    if [ -f "$PREFIX/$f" ]; then
        echo "  ok    $f"
    else
        echo "  MISS  $f"
        ok=0
    fi
done
if [ "$ok" != 1 ]; then
    echo "setup-toolchain.sh: prefix incomplete" >&2
    exit 1
fi
echo
echo "done. build with:  CXXROOT=$PREFIX ./build.sh"
