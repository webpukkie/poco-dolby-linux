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

Not done yet:

- [ ] PipeWire filter node integration (design below)
- [ ] Runtime parameter control from the host (the DAP parameter list via
      `DapEffectContext::setParamValues`, param 0x00)
- [ ] Formats beyond s16 stereo (float32, multichannel), rate changes at runtime
- [ ] Native x86_64 execution (currently aarch64 under qemu-user; qemu adds
      roughly 5-20× CPU overhead — a real-time filter will need the x86_64
      Dolby libs from another device, or an arm64 SBC)

## Quick start

Needs: `qemu-user` (aarch64), an aarch64 cross GCC + sysroot, and an aarch64
LLVM libc++/libunwind (the Dolby libs are linked against libc++).  See
`build.sh` for the expected env vars (`CC`, `SYSROOT`, `CXXROOT`).

```sh
./build.sh        # builds ./stage (stubs + deversioned libs) and ./probe
LOGLEVEL=2 ./run-probe.sh 48000 2 1 1 512 50
```

Expected output: `INIT/SET_CONFIG/.../ENABLE -> 0`, then blocks with `rc=0`,
and `DONE blocks=50 failures=0`.  With `LOGLEVEL=7` the library's own log
stream shows the whole internal pipeline (`DlbDlbBufferAdapter`,
`DlbDap2Process`, `DlbCrossfadeProcess` state transitions).

## Layout

| file | purpose |
|------|---------|
| `probe.c` | drives libswdap through the AOSP effect interface (create/command/process) |
| `aosp_effect_abi.h` | minimal AOSP audio-effect ABI structs (64-bit Android layout) |
| `apply_packed.c/.h` | applies the APS2 packed relocations at runtime after dlopen |
| `deversion.py` | strips Android symbol versioning so glibc can load the libs |
| `gen_stubs.py`, `extra_stubs.c` | generate + hand-fix the Android platform stub libs (`stub_auto.c` is generated into `stage/`) |
| `mkstage.sh`, `build.sh` | assemble `stage/` and build the probe |
| `run-probe.sh` | qemu-aarch64 invocation with the Android property env vars |
| `tools/rsp.py` | minimal GDB RSP client for `qemu-aarch64 -g` |
| `tools/dbg7.py` | breakpoint tracer used to root-cause the -38 (see RE-notes) |
| `docs/RE-notes.md` | the full reverse-engineering reference |

## PipeWire integration design (next step)

The library is a drop-in AOSP effect: one `create`, a handful of commands,
and a per-block `process(in, out)` with the block-size constraint from
`docs/RE-notes.md`.  A PipeWire filter node (SPA "audiofilter" style, or a
`pipewire-filter` module node) can wrap it:

1. **Loader:** dlopen `stage/libswdap.so`, apply packed relocations, create
   the effect once (uuid from `configs/audio_effects.xml`), send the
   INIT/SET_CONFIG/SET_DEVICE/SET_AUDIO_MODE/gain-sync/ENABLE sequence from
   `probe.c`.
2. **Graph:** `input stream → dolby filter node → output/sink`.  The node
   claims the same format it configures the effect with (start with s16
   stereo 48 kHz, 256-frame quantum — the engine's native block size at
   48 kHz; resample in PipeWire otherwise).
3. **Buffers:** feed `process()` from the SPA buffer; the input buffer gets
   clobbered once the crossfade is active, so copy the input into a scratch
   block first (see RE-notes).
4. **Emulation:** the aarch64 libs currently require `qemu-aarch64` (the
   whole node can run under `qemu-aarch64` + PipeWire, or the effect can be
   wrapped in a small aarch64 RPC shim driven by a native PipeWire node).
   For a native x86_64 setup, extract the x86_64 Dolby libs from an
   x86_64 Android image and repeat the same harness (the stubs and the
   relocation applier are already architecture-independent).
5. **Control:** map PipeWire/ALSA-volume changes to DAP params once
   `setParamValues` (param 0x00) is understood; see RE-notes for the
   parser entry points.
