#!/bin/sh
# Build the native side of the PipeWire integration:
#   dolby-rpc-client  — test client (stdin s16le PCM -> dolbyd -> stdout)
#   pw-dolby          — PipeWire filter node (needs PipeWire dev headers)
#
# PipeWire headers are looked up in this order:
#   $PW_PREFIX/{include,lib}   (explicit override)
#   system paths (pkg-config not required)
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
cd "$DIR"

PW_PREFIX="${PW_PREFIX:-}"
if [ -n "$PW_PREFIX" ]; then
    INC="-I$PW_PREFIX/include/pipewire-0.3 -I$PW_PREFIX/include/spa-0.2"
    LIB=""
    for d in "$PW_PREFIX/lib/$(gcc -dumpmachine)" "$PW_PREFIX/lib"; do
        [ -d "$d" ] && LIB="$LIB -L$d -Wl,-rpath,$d"
    done
fi

gcc -O2 -o dolby-rpc-client dolby-rpc-client.c
echo "built ./dolby-rpc-client"

echo '#include <pipewire/pipewire.h>
int main(void) { return 0; }' > /tmp/pwtest.c
HAVE_PW=0
if [ -n "$PW_PREFIX" ]; then
    gcc -O2 $INC -o /tmp/pwtest /tmp/pwtest.c $LIB -lpipewire-0.3 2>/dev/null && HAVE_PW=1
elif echo '#include <pipewire/pipewire.h>' | gcc -O2 -fsyntax-only -xc - 2>/dev/null; then
    HAVE_PW=1
fi

if [ "$HAVE_PW" = 1 ]; then
    gcc -O2 ${INC:-} -o pw-dolby pw-dolby.c ${LIB:-} -lpipewire-0.3
    echo "built ./pw-dolby"
else
    echo "PipeWire dev headers not found - skipping pw-dolby."
    echo "Set PW_PREFIX to a prefix containing include/pipewire-0.3 and include/spa-0.2."
fi
