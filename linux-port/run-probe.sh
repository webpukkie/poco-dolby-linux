#!/bin/sh
# Run the probe under qemu-aarch64 with the Android property env vars the
# Dolby libs expect.  Any args are passed through to ./probe.
#
#   PROP_persist.vendor.dolby.loglevel=7   verbose Dolby logs (0..7)
#   PROP_ro.vendor.audio.device.db=...     tuning database selection
cd "$(dirname "$0")"
QEMU="${QEMU:-qemu-aarch64}"
command -v "$QEMU" >/dev/null 2>&1 || {
    for d in "/tmp/crossroot/usr/bin" "/usr/bin"; do
        [ -x "$d/qemu-aarch64" ] && QEMU="$d/qemu-aarch64" && break
    done
}
exec "$QEMU" -L /tmp/crossroot/usr/aarch64-linux-gnu \
    -E LD_LIBRARY_PATH="$(pwd)/stage" \
    -E PROP_persist.vendor.dolby.loglevel="${LOGLEVEL:-7}" \
    -E PROP_ro.vendor.audio.device.db=DB_XM \
    -E PROP_ro.vendor.audio.device.dbcom=dolby_common \
    ./probe "$@"
