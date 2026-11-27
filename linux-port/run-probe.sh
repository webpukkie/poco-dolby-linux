#!/bin/sh
# Run the probe under qemu-aarch64 with the Android property env vars the
# Dolby libs expect.  Any args are passed through to ./probe.
#
#   PROP_persist.vendor.dolby.loglevel=7   verbose Dolby logs (0..7)
#   PROP_ro.vendor.audio.device.db=...     tuning database selection
cd "$(dirname "$0")"
SYS="${SYSROOT:-}"
if [ -z "$SYS" ] && [ -d /tmp/crossroot/usr/aarch64-linux-gnu/lib ]; then
    SYS=/tmp/crossroot
fi
QSYS="${SYS:-/usr/aarch64-linux-gnu}"

QEMU="${QEMU:-}"
if [ -z "$QEMU" ]; then
    for q in qemu-aarch64 qemu-aarch64-static; do
        command -v "$q" >/dev/null 2>&1 && QEMU="$q" && break
    done
fi
if [ -z "$QEMU" ]; then
    for d in /tmp/crossroot/usr/bin /usr/bin /usr/local/bin; do
        for q in qemu-aarch64 qemu-aarch64-static; do
            if [ -x "$d/$q" ]; then QEMU="$d/$q"; break; fi
        done
        [ -n "$QEMU" ] && break
    done
fi
[ -n "$QEMU" ] || { echo "qemu-aarch64 not found (install qemu-user-static)" >&2; exit 1; }

exec "$QEMU" -L "$QSYS" \
    -E LD_LIBRARY_PATH="$(pwd)/stage" \
    -E PROP_persist.vendor.dolby.loglevel="${LOGLEVEL:-7}" \
    -E PROP_ro.vendor.audio.device.db=DB_XM \
    -E PROP_ro.vendor.audio.device.dbcom=dolby_common \
    ./probe "$@"
