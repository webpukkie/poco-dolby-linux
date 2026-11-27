#!/bin/sh
# Start the Dolby RPC daemon (if not running) and the Dolby Atmos virtual sink.
#
# After it starts, select the "Dolby Atmos (DAP)" sink as your output device,
# e.g.:  wpctl set-default <id-of-dolby_atmos>   (or use your desktop's audio
# settings).  No pw-link wiring is needed.
#
# The real output device is detected automatically (the sink that was default
# before this one).  Override with DOLBY_SINK=<sink-name>, e.g. to chain into
# LiteFX:  DOLBY_SINK=litefx_sink ./run-pw-dolby.sh
cd "$(dirname "$0")"/..

SOCKET="${DOLBY_SOCKET:-/tmp/dolbyd.sock}"
pkill -x qemu-aarch64 2>/dev/null
pkill -x pw-dolby 2>/dev/null
rm -f "$SOCKET"
./run-daemon.sh "$SOCKET" &
sleep 0.5

SINK="$DOLBY_SINK"
CACHE="$HOME/.cache/dolby-linux-sink"
if [ -z "$SINK" ]; then
    SINK=$(pactl get-default-sink 2>/dev/null)
fi
case "$SINK" in
    ""|dolby_atmos*|*dolby_atmos*)
        # our own sink is (already) default - reuse the last real device
        SINK=$(cat "$CACHE" 2>/dev/null)
        ;;
    *)
        echo "$SINK" > "$CACHE"
        ;;
esac

if [ -n "$SINK" ]; then
    exec pipewire/pw-dolby --socket "$SOCKET" --sink "$SINK"
else
    echo "run-pw-dolby: no output sink detected; start with DOLBY_SINK=<sink-name>" \
        "(see: pactl get-default-sink)" >&2
    exec pipewire/pw-dolby --socket "$SOCKET"
fi
