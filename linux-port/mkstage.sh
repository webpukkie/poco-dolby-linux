#!/bin/sh
# Build the load stage: Android platform stubs + deversioned Dolby libs.
# Usage: mkstage.sh <dir-with-original-dolby-libs> <output-stage-dir> [CC] [SYSROOT]
set -e
SRC="${1:-.}"
OUT="${2:-stage}"
CC="${3:-aarch64-linux-gnu-gcc}"
SYS="${4:-/tmp/crossroot}"
CPP="$SYS/usr/aarch64-linux-gnu/lib"          # glibc for arm64
CXXROOT="${CXXROOT:-/tmp/crossroot-arm64}"    # LLVM libc++ for arm64

mkdir -p "$OUT"
cd "$OUT"

python3 ../gen_stubs.py "$SRC"

STUBS="liblog libutils libcutils libaudioutils libhidlbase libstagefright_foundation libxml2 vendor.dolby.hardware.dms@2.0"
for s in $STUBS; do
    $CC --sysroot="$SYS" -O2 -fPIC -shared -Wl,-soname,"$s.so" -o "$s.so" stub_auto.c
done

for lib in libswdap libdapparamstorage libdlbpreg libspatializerparamstorage; do
    cp "$SRC/$lib.so" "$lib.so"
    case "$lib" in
        libswdap) python3 ../deversion.py "$lib.so" ;;
        *)        python3 ../deversion.py "$lib.so" --no-init ;;
    esac
done

ln -sf "$CXXROOT/usr/lib/aarch64-linux-gnu/libc++.so.1.0" libc++.so.1
ln -sf libc++.so.1 libc++.so
ln -sf "$CXXROOT/usr/lib/aarch64-linux-gnu/libc++abi.so.1.0" libc++abi.so.1
ln -sf "$CXXROOT/usr/lib/llvm-19/lib/libunwind.so.1.0" libunwind.so.1
ln -sf "$CXXROOT/usr/lib/aarch64-linux-gnu/libunwind.so.8.1.0" libunwind.so.8
ln -sf "$CPP/libc.so.6" libc.so
ln -sf "$CPP/libm.so.6" libm.so
ln -sf "$CPP/libdl.so.2" libdl.so

echo "stage built in $OUT"
