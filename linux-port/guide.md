# Full setup guide — Dolby DAP/Atmos effect on x86_64 Linux (PipeWire)

This guide takes you from a fresh Arch Linux x86_64 machine (works on any
x86_64 distro with minor package-name changes) to hearing audio processed by
the actual Dolby DAP engine from a Poco X6 Pro through PipeWire.

**Read this first — what you get and what you don't:**

- The Dolby binaries in this repo (`binaries/arm64/*.so`) are **aarch64
  (ARM64) machine code**. There are no x86_64 Dolby libs in the repo, and
  `apply_packed.c` (which patches the packed APS2 relocations inside
  `libswdap.so`) has offsets hardcoded for this exact aarch64 build. So on
  your Ryzen the proprietary code runs under **`qemu-aarch64` user-mode
  emulation**. It works: measured throughput is ~10× realtime (509 ms CPU for
  5.12 s of 48 kHz stereo), so a real-time filter has plenty of headroom.
  Engine latency is ~23 ms at 48 kHz plus one block.
- What runs is the **DAP2 effect engine** (the post-processing insert: the
  virtualizer/surround/dialogue/EQ pipeline behind the "Dolby Atmos" toggle
  on the phone) applied to **already-decoded PCM**. On a PC your sources are
  decoded PCM anyway (every player/sink hands PCM to PipeWire), so an effect
  insert like this is exactly where DAP would sit — but this is **not** the
  object-based Atmos renderer that mixes spatial audio objects; that
  renderer isn't part of these libraries.

## 1. Requirements

Arch packages (the repo branch with the port lives in PR #1):

```sh
sudo pacman -S --needed base-devel git python curl \
    aarch64-linux-gnu-gcc qemu-user-static \
    pipewire pipewire-audio pipewire-pulse wireplumber
```

- `aarch64-linux-gnu-gcc` pulls in `aarch64-linux-gnu-binutils` and
  `aarch64-linux-gnu-glibc` (the target libc) automatically.
- `qemu-user-static` provides `/usr/bin/qemu-aarch64-static` (there is no
  plain `qemu-aarch64` on Arch; the run scripts try both names).
- `bsdtar` (part of `libarchive`, already installed on any Arch system) is
  used to unpack the Debian `.deb` files in the next step.
- PipeWire headers ship inside the main `pipewire` package on Arch, so
  `pipewire/build-pw.sh` will find them without extra installs.

## 2. Get the code

```sh
git clone https://github.com/webpukkie/poco-dolby-linux
cd poco-dolby-linux
git fetch origin pull/1/head:vibe-dolby
git checkout vibe-dolby
cd linux-port
```

(Once PR #1 is merged, the plain `main` branch is enough.)

## 3. Assemble the aarch64 libc++/libunwind prefix

The Dolby libs are C++ and were built against LLVM's libc++. Your cross GCC
provides target glibc but not libc++, so one script downloads the five
Debian trixie arm64 packages that contain them and unpacks them into
`~/.local/dap-cross` (no root needed):

```sh
./setup-toolchain.sh            # optional arg: prefix, default ~/.local/dap-cross
```

It fetches `libc++-19-dev`, `libc++1-19`, `libc++abi1-19`,
`libunwind-19-dev`, `libunwind-19` (LLVM 19, arm64) and `libunwind8`
(arm64 glibc unwinder) from `deb.debian.org`, extracts them, and verifies
the four files the build symlinks:

```
usr/lib/aarch64-linux-gnu/libc++.so.1.0
usr/lib/aarch64-linux-gnu/libc++abi.so.1.0
usr/lib/aarch64-linux-gnu/libunwind.so.8.1.0
usr/lib/llvm-19/lib/libunwind.so.1.0
```

Re-running it is safe (it just re-extracts).

## 4. Build

```sh
./build.sh
```

With a distro-installed cross gcc that's all: `build.sh` detects the
compiler's own sysroot, defaults `CXXROOT` to `~/.local/dap-cross`, and
builds

- `stage/` — the deversioned Dolby libs + Android platform stubs + `extra_stubs.so`
- `probe` — the self-test binary
- `pipewire/dolbyd` — the RPC daemon that actually hosts the effect

Environment variables (only needed if autodetection guesses wrong):

| var        | meaning                                          | default on Arch               |
|------------|--------------------------------------------------|-------------------------------|
| `CC`       | cross compiler                                   | `aarch64-linux-gnu-gcc`       |
| `SYSROOT`  | cross sysroot (empty = compiler's built-in one)  | empty                         |
| `CXXROOT`  | aarch64 libc++ prefix from step 3                | `~/.local/dap-cross`          |
| `QEMU`     | emulator binary                                  | `qemu-aarch64` or `qemu-aarch64-static` |

## 5. Verify the engine

```sh
LOGLEVEL=2 ./run-probe.sh 48000 2 1 1 512 30
```

Arguments: `rate channels format enable frames blocks`.  You should see the
AOSP effect commands succeed (`INIT/SET_CONFIG/.../ENABLE -> 0`), 30 blocks
of `rc=0`, and the line:

```
DONE blocks=30 failures=0
```

Try more stress if you like: `LOGLEVEL=2 ./run-probe.sh 44100 2 1 1 256 200`.
With `LOGLEVEL=7` you get the library's internal log stream (`DlbDap2Process`,
`DlbCrossfadeProcess` state transitions) — useful, but verbose.

## 6. First real audio — CLI filter

The RPC daemon runs the aarch64 engine under qemu; the native client pipes
raw s16le PCM through it:

```sh
./run-daemon.sh &                      # listens on /tmp/dolbyd.sock
ffmpeg -i music.wav -f s16le -ar 48000 -ac 2 - \
    | ./pipewire/dolby-rpc-client 48000 2 512 > dap.raw
ffmpeg -f s16le -ar 48000 -ac 2 -i dap.raw dap-out.wav
kill %1
```

`dolby-rpc-client` args are `rate channels frames` (frames must be ≥ the
engine's block size; 512 is safe at 48 kHz). The last partial block is
zero-padded. Compare `dap-out.wav` with the original — the engine adds its
~23 ms delay and the DAP processing (audible phase/filter differences even
with default tuning).

## 7. Live in the PipeWire graph

Build the PipeWire node (links against your system PipeWire):

```sh
./pipewire/build-pw.sh
./pipewire/run-pw-dolby.sh             # starts dolbyd + pw-dolby
```

This creates a **virtual sink** named `dolby_atmos` that shows up as
"Dolby Atmos (DAP)" in your desktop's audio settings and in `wpctl status`
under Sinks. No `pw-link` wiring is needed: everything routed to this sink
is processed by the DAP engine and played back on your real output device
(automatically detected — the sink that was default before `dolby_atmos`
started; override with `DOLBY_SINK=<sink-name> ./pipewire/run-pw-dolby.sh`).

Make it your default output and play something:

```sh
wpctl status                 # find the dolby_atmos node id under Sinks, e.g. 123
wpctl set-default 123
# or pick "Dolby Atmos (DAP)" in your desktop audio settings / pavucontrol
```

To chain into an existing DSP (e.g. LiteFX manages its own virtual-sink
chain): `DOLBY_SINK=litefx_sink ./pipewire/run-pw-dolby.sh` — the Dolby
output then feeds LiteFX's sink instead of the hardware.

The node runs at 48 kHz with 256-frame engine blocks by default; the DAP
engine takes s16 internally (conversion and resampling are handled by the
PipeWire stream converters, so apps at other rates/formats are fine).
Match `--rate` to your graph rate for best quality
(`pw-metadata -n settings | grep clock.rate`, default 48000).

`run-pw-dolby.sh` passes extra flags through: `--rate 44100 --frames 1024
--socket /tmp/my.sock --name my_dolby` all work (raise `--frames` if you get
dropouts; 1024 doubles the per-block time budget).

## 8. Troubleshooting

- **`DONE blocks=N failures=0` is the only healthy result.** Any `rc=-38`
  means the crossfade priming fix didn't apply — make sure you built from
  the PR branch and didn't mix in an old `stage/` (`rm -rf stage && ./build.sh`).
- **Verbose engine logs:** `LOGLEVEL=0..7` (env var) on `run-probe.sh` /
  `run-daemon.sh` / `run-pw-dolby.sh`; 0 is quiet, 7 traces everything.
  `No cache defined for device 0x40000000` lines are benign.
- **Socket already in use / daemon won't start:** one connection at a time;
  `rm -f /tmp/dolbyd.sock` and make sure no old daemon lingers:
  `pkill -x qemu-aarch64` (kill by exact process name — never `pkill -f`).
- **Links fail with `failed to link ports`** — no longer applies: the
  virtual-sink design needs no manual links. If you still run an older
  filter-node build, upgrade to the current branch.
- **No sound, but `wpctl status` shows the Dolby sink and a `dolby_atmos_output`
  stream** — check the stream's target: `pw-cli info` on the output stream's
  node id; if it is linked to the wrong device, restart with
  `DOLBY_SINK=<sink-name> ./pipewire/run-pw-dolby.sh`.
- **Feedback loop / echo** — never set `--sink` to the Dolby sink itself;
  the script caches the last real device in `~/.cache/dolby-linux-sink` to
  avoid this after restarts.
- **Silence at start for ~10 ms** — the output stream primes two engine
  blocks before emitting, to avoid crackle from pipeline jitter.
- **Crackling/dropouts in the live node:** the qemu round-trip costs ~1 ms
  per 256-frame block; raise `--frames` to 512/1024, and keep the machine
  out of deep powersave.
- **Latency:** ~23 ms engine delay + one block of buffering (512 frames =
  10.7 ms at 48 kHz). This is inherent to the phone's audio path.
- **`mkstage.sh: cannot locate aarch64 glibc`** — your cross gcc is
  unusual; set `SYSROOT` to the directory containing `usr/aarch64-linux-gnu/lib`.
- **`setup-toolchain.sh` 404s** — Debian rebuilt the packages (new `+bN`
  suffix). Take the current filenames from
  `https://deb.debian.org/debian/pool/main/l/llvm-toolchain-19/` and
  `.../pool/main/libu/libunwind/`, or export `DEBS`-style URLs by editing
  the list at the top of the script.
- **Input clobbering:** the engine writes into its input buffer while
  fading in. The daemon and the PipeWire node already use a scratch buffer,
  so callers don't see this; only relevant if you write your own client.

## 9. Environment variable contract (for custom setups)

The Android property lookups are mapped to `PROP_*` env vars and set for you
by the run scripts: `PROP_persist.vendor.dolby.loglevel` (= `LOGLEVEL`),
`PROP_ro.vendor.audio.device.db=DB_XM`, `PROP_ro.vendor.audio.device.dbcom=dolby_common`.
If you invoke `qemu-aarch64 -L <sysroot> ... stage` yourself, pass these with
`-E` and `LD_LIBRARY_PATH=<repo>/linux-port/stage`.

## 10. Known limits / honest scope

- s16 stereo (mono/surround layouts and float32 not wired up); rate fixed at
  node creation (PipeWire resamples the graph to match).
- No runtime parameter control yet (virtualizer strength, EQ, dialogue
  enhancer are whatever the device's default tuning blob selects). This is
  the next planned piece (`DapEffectContext::setParamValues`).
- aarch64 under qemu: correct but emulated. Native x86_64 needs x86_64 Dolby
  libraries from another device dump plus a generalized relocation applier.
- Legal note: the Dolby `.so` files are proprietary; this harness only makes
  them loadable for interoperability/personal use on hardware that shipped
  with them.
