#!/bin/sh
# Run the Dolby RPC daemon (aarch64) under qemu-aarch64.
# Usage: ./run-daemon.sh [socket-path]     (default /tmp/dolbyd.sock)
cd "$(dirname "$0")"
QEMU="${QEMU:-qemu-aarch64}"
command -v "$QEMU" >/dev/null 2>&1 || {
    for d in "/tmp/crossroot/usr/bin" "/usr/bin"; do
        [ -x "$d/qemu-aarch64" ] && QEMU="$d/qemu-aarch64" && break
    done
}
exec "$QEMU" -L /tmp/crossroot/usr/aarch64-linux-gnu \
    -E LD_LIBRARY_PATH="$(pwd)/stage" \
    -E PROP_persist.vendor.dolby.loglevel="${LOGLEVEL:-2}" \
    -E PROP_ro.vendor.audio.device.db=DB_XM \
    -E PROP_ro.vendor.audio.device.dbcom=dolby_common \
    ./pipewire/dolbyd "${1:-/tmp/dolbyd.sock}"
