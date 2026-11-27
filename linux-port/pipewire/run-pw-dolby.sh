#!/bin/sh
# Start the Dolby RPC daemon (if not running) and the PipeWire filter node.
#
# After the node is running, route audio into it, e.g.:
#   pw-link <source-output-port> dolby_atmos:input_FL
#   pw-link <source-output-port> dolby_atmos:input_FR
#   pw-link dolby_atmos:output_FL <sink-playback-FL>
#   pw-link dolby_atmos:output_FR <sink-playback-FR>
# or set this node as a target in your player (it advertises itself as
# "dolby_atmos", media class Filter/DSP).
cd "$(dirname "$0")"/..
./run-daemon.sh "$@" &
sleep 0.5
exec pipewire/pw-dolby --socket "${1:-/tmp/dolbyd.sock}"
