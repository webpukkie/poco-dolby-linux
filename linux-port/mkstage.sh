#!/bin/sh
# Build the load stage: Android platform stubs + deversioned Dolby libs.
# Usage: mkstage.sh <dir-with-original-dolby-libs> <output-stage-dir> [CC] [SYSROOT]
#   SYSROOT empty (4th arg unset/empty) means: use the compiler's own sysroot.
# Env:
#   CXXROOT  aarch64 LLVM libc++/libunwind prefix (see setup-toolchain.sh)
set -e
SRC="${1:-.}"
OUT="${2:-stage}"
CC="${3:-aarch64-linux-gnu-gcc}"
SYS="${4:-}"
if [ -n "$SYS" ]; then
    CPP="$SYS/usr/aarch64-linux-gnu/lib"      # glibc for arm64
else
    CPP=$(dirname "$("$CC" -print-file-name=libc.so.6 2>/dev/null)")
fi
if [ ! -f "$CPP/libc.so.6" ]; then
    echo "mkstage.sh: cannot locate aarch64 glibc (libc.so.6)" >&2
    echo "  tried: $CPP" >&2
    exit 1
fi
CXXROOT="${CXXROOT:-}"
if [ -z "$CXXROOT" ]; then
    if [ -d /tmp/crossroot-arm64/usr/lib/llvm-19 ]; then
        CXXROOT=/tmp/crossroot-arm64
    else
        CXXROOT="$HOME/.local/dap-cross"
    fi
fi

mkdir -p "$OUT"
cd "$OUT"

# tell gen_stubs.py where the target glibc and libc++ live, so it does not
# emit stubs for symbols the real libraries provide
export CPPLIB="$CPP"
export CXXLIB="$CXXROOT/usr/lib/aarch64-linux-gnu"
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
if [ ! -f "$CXXROOT/usr/lib/llvm-19/lib/libunwind.so.1.0" ] && [ -f "$CXXROOT/usr/lib/aarch64-linux-gnu/libunwind.so.8.1.0" ]; then
    ln -sf "$CXXROOT/usr/lib/aarch64-linux-gnu/libunwind.so.8.1.0" libunwind.so.1
else
    ln -sf "$CXXROOT/usr/lib/llvm-19/lib/libunwind.so.1.0" libunwind.so.1
fi
ln -sf "$CXXROOT/usr/lib/aarch64-linux-gnu/libunwind.so.8.1.0" libunwind.so.8
ln -sf "$CPP/libc.so.6" libc.so
ln -sf "$CPP/libm.so.6" libm.so
ln -sf "$CPP/libdl.so.2" libdl.so

echo "stage built in $OUT"
