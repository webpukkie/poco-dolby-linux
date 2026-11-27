# linux-port — Dolby Atmos (DAP) running on Linux

This directory contains a working harness that loads the **actual proprietary
Dolby DAP library** (`binaries/arm64/libswdap.so`, aarch64, from a Poco X6 Pro)
on Linux, drives it through the exact AOSP audio-effect command interface, and
gets real DSP output out of it — verified end-to-end under `qemu-aarch64`.

## Status

Working (verified, see `docs/RE-notes.md` for the full analysis):

- [x] libswdap.so + libdapparamstorage.so + libdlbpreg.so +
      libspatializerparamstorage.so load and run on Linux/glibc
      (deversioning + APS2 packed-relocation applier + Android platform stubs)
- [x] Full effect lifecycle: `create → INIT → SET_CONFIG → SET_DEVICE →
      SET_AUDIO_MODE → ENABLE → process` with rc=0
- [x] Real DSP processing confirmed: the DlbDap2Process engine
      (`dap_cpdp_prepare`/`dap_cpdp_process`) transforms the input
      (filter delay/phase shift, amplitude changes) instead of passing it through
- [x] Stable: 200+ blocks at 44.1/48 kHz, block sizes 256/512/1024 s16 stereo,
      zero crashes, no library modifications
- [x] The `-ENOSYS` (-38) deadlock root-caused and fixed (pregain/crossfade
      priming, see `docs/RE-notes.md`)
- [x] PipeWire virtual sink (`pipewire/pw-dolby`) + RPC daemon
      (`pipewire/dolbyd`): audio flows from a PipeWire graph through the real
      DAP engine and back; ~10× realtime headroom under qemu (measured:
      509 ms CPU for 5.12 s of 48 kHz stereo)

Not done yet:

- [ ] Runtime parameter control from the host (the DAP parameter list via
      `DapEffectContext::setParamValues`, param 0x00)
- [ ] Formats beyond s16 stereo (float32, multichannel), rate changes at runtime
- [ ] Native x86_64 execution (currently aarch64 under qemu-user; qemu adds
      roughly 5-20× CPU overhead — a real-time filter will need the x86_64
      Dolby libs from another device, or an arm64 SBC)

## Quick start

Full step-by-step walkthrough for an x86_64 desktop (Arch Linux and friends):
**[guide.md](guide.md)** — packages, toolchain prefix, build, probe, CLI
filter, live PipeWire graph, troubleshooting.

Short version: run `./setup-toolchain.sh` once (fetches the aarch64 LLVM
libc++/libunwind the Dolby libs need), then:

```sh
./build.sh        # builds ./stage (stubs + deversioned libs) and ./probe
LOGLEVEL=2 ./run-probe.sh 48000 2 1 1 512 50
```

Expected output: `INIT/SET_CONFIG/.../ENABLE -> 0`, then blocks with `rc=0`,
and `DONE blocks=50 failures=0`.  With `LOGLEVEL=7` the library's own log
stream shows the whole internal pipeline (`DlbDlbBufferAdapter`,
`DlbDap2Process`, `DlbCrossfadeProcess` state transitions).

## Using it as a PipeWire effect

Architecture: the aarch64 Dolby libs run in `dolbyd` (a small RPC daemon,
under `qemu-aarch64`), and a native PipeWire virtual sink (`pipewire/pw-dolby`)
shuttles PCM blocks to it over a Unix socket and plays the result on your
real output device.

```sh
cd pipewire && PW_PREFIX=<pipewire-prefix> ./build-pw.sh   # or plain ./build-pw.sh for system PipeWire
cd ..
./pipewire/run-pw-dolby.sh                 # starts dolbyd + the virtual sink
```

Then select the **"Dolby Atmos (DAP)"** sink as your output device (desktop
audio settings / pavucontrol, or `wpctl set-default <id>`) — no `pw-link`
wiring is needed.  Everything played into it is processed by the DAP engine
and played on your real output device (auto-detected, or force it with
`DOLBY_SINK=<sink-name> ./pipewire/run-pw-dolby.sh`; use e.g.
`DOLBY_SINK=litefx_sink` to chain into an existing DSP).  The streams are
f32 at 48 kHz stereo by default (the DAP engine runs s16 internally;
`--rate/--frames/--socket/--name` to change — PipeWire's stream converters
handle every app format and rate).

Without a running session you can still exercise the exact same data path
from the command line (raw s16le PCM on stdin/stdout):

```sh
./run-daemon.sh &
ffmpeg -i in.wav -f s16le -ar 48000 -ac 2 - | \
    pipewire/dolby-rpc-client 48000 2 512 > out.raw
ffmpeg -f s16le -ar 48000 -ac 2 -i out.raw out.wav
```

Latency: one graph quantum of DSP delay (~23 ms at 48 kHz, inherent to the
effect) plus the RPC round-trip.  The daemon side costs ~1 ms per 512-frame
block under qemu (10× realtime headroom on a modern x86_64 core), so the
blocking RPC inside `on_process()` is acceptable for a desktop demo; run
`dolbyd` natively on arm64 (same binary, no qemu) for production-grade
latency.  Known engine quirks: the input block is clobbered by `process()`
(handled by the scratch copy in both frontends) and `GET_CONFIG` needs
`cmdSize >= 112` (see `docs/RE-notes.md`).

## Layout

| file | purpose |
|------|---------|
| `probe.c` | self-test: synthetic tone through the full effect interface |
| `dap_effect.c/.h` | reusable wrapper: engine load + effect lifecycle + process |
| `aosp_effect_abi.h` | minimal AOSP audio-effect ABI structs (64-bit Android layout) |
| `apply_packed.c/.h` | applies the APS2 packed relocations at runtime after dlopen |
| `deversion.py` | strips Android symbol versioning so glibc can load the libs |
| `gen_stubs.py`, `extra_stubs.c` | generate + hand-fix the Android platform stub libs (`stub_auto.c` is generated into `stage/`) |
| `mkstage.sh`, `build.sh` | assemble `stage/` and build the probe + daemon |
| `run-probe.sh` | qemu-aarch64 invocation with the Android property env vars |
| `run-daemon.sh` | run `pipewire/dolbyd` under qemu with the property env vars |
| `pipewire/dolbyd.c` | RPC daemon: drives the DAP, serves PCM blocks over a Unix socket |
| `pipewire/dolby-rpc-client.c` | native test client (stdin PCM -> daemon -> stdout) |
| `pipewire/pw-dolby.c` | native PipeWire virtual sink (pw_stream API, f32↔s16, stereo) |
| `pipewire/build-pw.sh`, `pipewire/run-pw-dolby.sh` | build the native side / launch daemon + node |
| `tools/rsp.py` | minimal GDB RSP client for `qemu-aarch64 -g` |
| `tools/dbg7.py` | breakpoint tracer used to root-cause the -38 (see RE-notes) |
| `docs/RE-notes.md` | the full reverse-engineering reference |

## PipeWire integration notes

Implemented (see "Using it as a PipeWire effect" above), following this
design:

1. **Loader:** `dap_effect.c` dlopens `stage/libswdap.so`, applies packed
   relocations, creates the effect (uuid from `configs/audio_effects.xml`)
   and sends the INIT/SET_CONFIG/SET_DEVICE/SET_AUDIO_MODE/gain-sync/ENABLE
   sequence.
2. **Graph:** `apps → dolby_atmos virtual sink (pw_stream, media.class
   Audio/Sink) → DAP engine → dolby_atmos_output playback stream → real
   sink`.  WirePlumber links the output stream to the device that was
   default at startup (or `--sink`), so no manual wiring is needed.  The
   streams advertise f32 / 48 kHz / stereo (the engine's s16 native block
   is 256 frames at 48 kHz; the server-side converters convert/resample).
3. **Buffers:** `pw-dolby` converts f32→s16 interleaved into a FIFO, pushes
   whole 256-frame blocks through the daemon (the input buffer gets
   clobbered once the crossfade is active, see RE-notes) and drains a
   second FIFO into the playback stream, primed two blocks deep to avoid
   crackle from pipeline jitter.
4. **Emulation:** the aarch64 libs run in `dolbyd` under `qemu-aarch64`
   while the PipeWire node stays native; on an arm64 host `dolbyd` runs
   natively with no qemu.  For a native x86_64 setup, extract the x86_64
   Dolby libs from an x86_64 Android image and repeat the same harness (the
   stubs and the relocation applier are architecture-independent).
5. **Control:** map PipeWire/ALSA-volume changes to DAP params once
   `setParamValues` (param 0x00) is understood; see RE-notes for the
   parser entry points.  (Not implemented yet; ENABLE/DISABLE already is,
   via RPC cmds 2/3.)
