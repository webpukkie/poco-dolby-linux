# Reverse-engineering notes — libswdap.so (Dolby DAP, Poco X6 Pro)

Everything below was established by running the real aarch64 library under
`qemu-aarch64` (user mode) with a hand-written GDB RSP client
(`tools/rsp.py`), software breakpoints/register pokes, and by reading the
library's own runtime logs (`PROP_persist.vendor.dolby.loglevel=7`).  All
addresses are vaddrs inside `binaries/arm64/libswdap.so` (the deversioned
copy in `stage/` has the same layout; `.rodata` vaddr == file offset).

## Loading on Linux/glibc

* The libs are Android ELF64 with symbol versioning (`DT_VERNEED`) and
  **APS2 packed relocations** (custom SHT-type format) — glibc's loader
  ignores both.  `deversion.py` strips the verneed; the packed relocations
  (952 of them in libswdap: 703× ABS64 type 257, 249× GLOB_DAT type 1025)
  are applied at runtime by `apply_packed.c` after `dlopen`, using the base
  address from `dlinfo(RTLD_DI_LINKMAP)`.
* Android platform dependencies (libbinder, libutils, libhidlbase, ...)
  are satisfied by stub libraries built from `stub_auto.c`
  (`gen_stubs.py` generates the symbol list).  Semantics-critical stubs
  (AOSP `VectorImpl`/`SortedVectorImpl`, working `String8`, `operator new`,
  HIDL `tryGetService` returning a null `sp<>` via the x8 sret slot) live in
  the functional prelude of `gen_stubs.py` so a regenerated `stub_auto.c`
  always contains them.  Stubs referenced only by the Android APS2-packed
  relocations live in `extra_stubs.c` (built as `stage/extra_stubs.so`, which
  the probe dlopens with `RTLD_GLOBAL` before applying the packed relocations).
* Android system properties are mapped to env vars: `PROP_<name>`.

## DlbEffect command interface (AOSP audio-effect ABI, vendor cmd codes)

`effectCommand` dispatches through a 21-entry 16-bit-offset jump table at
`.rodata 0x24398` (base `0x12ca48`).  Verified cmd codes:

| cmdCode | command               | notes                                              |
|---------|-----------------------|----------------------------------------------------|
| 0       | INIT                  |                                                    |
| 1       | SET_CONFIG            | cmdData = `effect_config_s` (112 bytes)            |
| 2       | RESET                 | no data                                            |
| 3       | ENABLE                | no data                                            |
| 4       | DISABLE               |                                                    |
| 5       | SET_PARAM             | see layout below                                   |
| 8       | GET_PARAM             | same layout, writes into reply                     |
| 9       | SET_DEVICE            | cmdData = `uint32 audio_devices_t`                 |
| 10      | SET_VOLUME            |                                                    |
| 11      | SET_AUDIO_MODE        | cmdData = `uint32 audio_mode_t`                    |
| 14      | GET_CONFIG            | cmdSize must be ≥ 112; reply = `effect_config_s`   |
| 20      | OFFLOAD               |                                                    |

`EFFECT_CMD_SET_PARAM` (5) payload (vendor layout, *not* plain AOSP
`effect_param_t`):

```
+0   int32  status            (ignored on set)
+4   uint32 must be 4         (validated!)
+8   uint32 vsize             (value size, passed to setParam)
+12  uint32 param id          (4-char tag, e.g. 'preg' = 0x67657270)
+16  value[vsize]
```
forwarded as `EffectContext::setParam(param, vsize, &value)`
(`_ZN5dolby13EffectContext8setParamEijPv`), which only understands
param **0x12** (→ `setBypass`); `DapEffectContext::setParam`
(`_ZN5dolby16DapEffectContext8setParamEijPv`, vtable-dispatched before it)
understands 0x00 (`setParamValues`, an `EffectParamParser` param list),
0x04 (`setSelectedTuningDevice`), 0x05 (`setDapParameter`), 0x13/0x14/0x15;
everything else returns -2 "unknown parameter".

## EffectDap object layout (`this` in EffectDap::*)

| offset | field                                                       |
|--------|-------------------------------------------------------------|
| +8     | pthread_mutex (locked by process/setParam)                  |
| +0x58  | EndpointParamCache                                          |
| +48    | enabled flag (byte)                                         |
| +49    | bypass flag (byte)                                          |
| +50    | crossfade-pending flag (byte)                               |
| +51    | config-changed flag (byte)                                  |
| +52    | "vbon" mirror (byte)                                        |
| +80    | DlbCrossfadeProcess\*                                       |
| +0x198 | input AudioBufferProvider                                   |
| +0x1d0 | output AudioBufferProvider                                  |
| +0x208 | lastGain / mPregain mirror (int, atomic `ldar`/`stlr`)      |
| +0x20c | mute/unmute flag (byte): 1 = unmute path A, 0 = mute path B |

`AudioBufferProvider` (+0x198/+0x1d0): +8 channel count, +32 pointer to a
per-channel pointer array, +40 frameCount, +48 per-channel byte stride.
`set(audio_buffer_s*)` at 0x1445b0 fills `arr[i] = raw + i*stride`.

## The priming / crossfade / -ENOSYS mechanics (the heart of the port)

`EffectDap::process` (0x12a4a0) per block:

1. `getParam(0x40000000 /* AUDIO_DEVICE_OUT_DEFAULT hardcoded */, 'preg', &newGain, &size)`
   against the EndpointParamCache; `lastGain = ldar [this+0x208]`.
2. If `lastGain == newGain` → skip priming (branch to 0x12a6f4).
3. Priming paths:
   * path A (unmute): `newGain ≥ -830 && lastGain ≤ -831` → `[0x20c]=1`
   * path B (mute):   `newGain ≤ -831 && lastGain ≥ -830` → `[0x20c]=0`
   * both: `crossfade->setEnabled(flags[48] ? (w24^1) : 0)` — **the argument
     is always 0 during priming**, so a crossfade in PREROLL_ACTIVE is always
     forced to PROCESS_BYPASSED;
   * then `EndpointParamCache::setParam(0x40000000, 'preg', &lastGain, 1)` +
     `commitChangedParams(0x40000000)` — this is the only writer of the
     pregain into the cache, and it forwards it to the DSP engine.
4. `AudioBufferProvider::set(in_prov, in)`, `set(out_prov, out)`, dumps.
5. `if (flags[50] | flags[51]) → crossfade->process(in_prov, out_prov)`
   else `return flags[49]==0 ? -38 (-ENOSYS) : 0`.
6. Crossfade `process` rc==0 → flags[50] **kept set**; rc≠0 → flags[50]=0,
   return rc unless bypassed.

`DlbCrossfadeProcess` state machine (state at +56; 0x13ca80 `setEnabled`,
0x139420 `deviceChanged`, 0x1394b0 `process`):

| state | name              | process (0x1394b0 jump table)                        |
|-------|-------------------|------------------------------------------------------|
| 4     | PROCESS_BYPASSED  | 0x139724: passthrough memcpy, **always returns -38**  |
| 0     | PREROLL_ACTIVE    | 0x1396b4: **main DSP path** — inner->process(in_prov, internal), then copy to out |
| 1     | FADE_TO_ACTIVE    | 0x139758                                              |
| 2     | (fade active)     | 0x1397a8                                              |
| 3     | FADE_OUT          | 0x1397f4                                              |

`setEnabled(enable)`: state4 + enable → state 0 (+ inner setEnabled(1));
state0 + disable → state 4; state2 + disable → state 3; state3 + disable → 4.

**Why naive runs return -38 on every block:** at INIT the wrapper sets
`mPregain = -2080` (speaker tuning) into `[0x208]`, but the commit fails
harmlessly ("PregainSynchronizer: Set Pregain Error" — no device cache
exists yet).  On the first block the cache is empty (`getParam` → 0), so
`lastGain(-2080) != newGain(0)` → priming → path A → crossfade
`setEnabled(0)` → state 0→4 → the effect returns -38 and clears
flags[50].  Every later block: no priming, flags[50]=0, flags[49]=0 →
-38 at 0x12a82c.  Nothing in the library ever leaves state 4 again
(re-enabling needs `[0x20c]==0`, which only the mute priming path writes).

**The fix used here (probe.c "gain-sync"):** before ENABLE, call
`EffectDap::setParam(dap, 0x40000000, 'preg', &zero, 1)` directly (object
walk: `iface+8` = EffectContext\*, `ctx+24` = EffectDap\*) so
`lastGain == 0 == empty-cache getParam value`.  The first block then skips
priming, the crossfade stays in PREROLL_ACTIVE, flags[50] (set by ENABLE)
persists, and every block takes the state-0 main DSP path and returns 0.

## DSP engine (DlbDap2Process)

* Block size is rate-dependent: `Dap2Process::process` reads `[x19+56]`;
  48000 Hz → 256 frames.  Input must be a multiple of the block size.
* Call chain: `DlbDlbBufferAdapter::process` (splits input into
  block-sized chunks) → `dap_cpdp_prepare` (0x159fe0) →
  `dap_cpdp_process` (0x15f8d8).
* The crossfade preroll phase outputs the input unchanged (passthrough
  while buffering); after `PREROLL_ACTIVE → FADE_TO_ACTIVE` real processed
  audio appears (verify: output samples show filter delay/phase shift vs
  the input sine).
* Once fading, the library writes the processed audio through **both**
  providers — the input buffer ends up holding the same processed data as
  the output buffer.  Hosts must not rely on the input buffer surviving a
  `process()` call; use a scratch input buffer.

## Debugging tools

`tools/rsp.py` is a minimal GDB RSP client for `qemu-aarch64 -g 1234`.
Protocol notes that cost us hours:

* ack every received packet with `+`, strip stray `+`/`-` (qemu resends
  the last packet otherwise);
* the `g` packet hex stream is little-endian in stream order
  (x0–x30 = chars 0..535, sp = 31st, pc = 32nd register, then pstate);
* after stopping at a software breakpoint, qemu re-reports the same stop
  on `c` — the client must single-step (`s`) over the breakpoint first;
* `Z0/Z2/z0/P<n>` all work in qemu-user; register numbers in `P` are hex.

## Verified working configuration

s16 interleaved stereo, 44100/48000 Hz, block sizes 256/512/1024, 200+
blocks, rc=0 throughout, under qemu-aarch64, with the gain-sync fix and
no library modifications.  Remaining gaps are listed in linux-port/README.md.
