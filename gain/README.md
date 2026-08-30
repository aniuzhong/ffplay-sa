# gain/ — volume gain algorithms experiment

`experiment/volume-gain` branch only. Ports external players' gain/volume
algorithms onto ffplay for comparison testing: VLC's replay gain (the
original experiment) and mpv's AO gain model. Player plugin machinery
(VLC `module_need` dispatch, mpv option/property plumbing) is intentionally
not ported — plain functions and env vars replace it.

## Layout

| Path | Origin | Role |
|---|---|---|
| `vlc/replay_gain.{h,c}` | VLC `include/vlc_replay_gain.h`, `src/input/replay_gain.c` | Tag model + multiplier calculation (pure functions) |
| `vlc/amplify.{h,c}` | VLC `modules/audio_mixer/{integer,float}.c` | Per-sample kernels (S16/S32/U8/FL32) |
| `vlc/volume.{h,c}` | VLC `src/audio_output/volume.c` | Lock-free gain factor state + kernel dispatch |
| `mpv/mpv_replaygain.{h,c}` | mpv `player/audio.c`, `demux/demux.c` | `audio_get_gain()` composition, replaygain computation, three-generation tag parsing (pure functions) |
| `mpv/mpv_kernels.{h,c}` | mpv `audio/out/ao.c` | Q8.8 fixed-point / float kernels (`process_plane()`, `ao_post_process_data()`) |
| `ffplay_gain.{h,c}` | new | VLC backend adapter: tag harvest, env config, SDL trampoline |
| `ffplay_gain_mpv.{h,c}` | new | mpv backend adapter: side-data/tag harvest, env config, SDL trampoline, capture hook |
| `tests/` | new | `test_replay_gain.c` (VLC math), `test_mpv_gain.c` (mpv math + kernels + mpv-vs-VLC cross table) |

## Backends

`FFPLAY_GAIN_BACKEND` (CMake, default `vlc`) selects which adapter the
injected ffplay build links; the two backends are never linked together.
Use a separate build dir to keep both binaries around:

```sh
cmake -B build-mpv -DFFPLAY_GAIN_BACKEND=mpv && cmake --build build-mpv
```

With no env vars set either backend is a no-op (unity gain).

### VLC backend (`FFPLAY_GAIN_*`)

| Variable | Default | Meaning |
|---|---|---|
| `FFPLAY_GAIN_MODE` | `none` | `track`, `album` or `none` (`audio-replay-gain-mode`) |
| `FFPLAY_GAIN_PREAMP` | `0` | pre-amplification dB (`audio-replay-gain-preamp`) |
| `FFPLAY_GAIN_DEFAULT` | `-7` | dB for files without tags (`audio-replay-gain-default`) |
| `FFPLAY_GAIN_PEAK_PROTECTION` | `1` | clamp multiplier to `1/peak` to avoid clipping |
| `FFPLAY_GAIN` | `1.0` | extra linear multiplier (VLC `gain`) |

### mpv backend (`FFPLAY_MPV_*`)

| Variable | Default | Meaning (mpv option) |
|---|---|---|
| `FFPLAY_MPV_VOLUME` | `100` | user volume percent, cubic curve (`--volume`) |
| `FFPLAY_MPV_VOLUME_GAIN` | `0` | extra gain in dB (`--volume-gain`) |
| `FFPLAY_MPV_MUTE` | `0` | mute (`--mute`) |
| `FFPLAY_MPV_REPLAYGAIN` | `none` | `track` / `album` / `none` (`--replaygain`) |
| `FFPLAY_MPV_RG_PREAMP` | `0` | pre-amplification dB (`--replaygain-preamp`) |
| `FFPLAY_MPV_RG_CLIP` | `0` | `1` = allow clipping (`--replaygain-clip`) |
| `FFPLAY_MPV_RG_FALLBACK` | `0` | dB when replaygain logic inactive; 0 = off (`--replaygain-fallback`) |
| `FFPLAY_MPV_CAPTURE` | unset | dump post-gain S16LE samples to file (for diffing against mpv reference output) |

Example: `FFPLAY_MPV_REPLAYGAIN=track FFPLAY_MPV_RG_PREAMP=2 ffplay file.flac`

### mpv port fidelity

Ported verbatim from mpv 0.41.0 (expression order and float/double types
preserved for bit-comparability): the total gain factor
`(volume/100)^3 * replaygain * 10^(dB/20)` with mute and clip prevention
`min(gain, 1/peak)`; tag parsing for classic `REPLAYGAIN_*`, new-style
`REPLAYGAIN_GAIN/PEAK` and Opus `R128_*` (Q7.8, with mpv's +5 dB
ReplayGain-2 vs EBU-R128 compensation); the Q8.8 fixed-point kernels
(`lrint(256*gain)` quantization, unity-skip, MPCLAMP). Replaygain data is
sourced with mpv's demuxer semantics — stream `AV_PKT_DATA_REPLAYGAIN`
side data, then stream tags, then file tags, first source wins.

Re-hosted with no numerical effect: mpv options -> env vars, C11 `_Atomic
float` -> the MSVC-safe lock-free pattern from `vlc/volume.h`, the AO
playback-thread application point -> the SDL trampoline.

Known divergences (by experiment design): ffplay volume keys still drive
SDL's mixer (keep it at 100% for gain tests); options are read once at
startup, not per property update; `--volume-max` clamping (mpv's property
layer) is not enforced.

**Verified sample-exact** against real mpv v0.41.0-1012-ge8673660a via
`--ao=pcm --ao-pcm-file=... --audio-format=s16` vs `FFPLAY_MPV_CAPTURE`:
byte-identical output for `--volume=100`, `--volume=70` and
`--replaygain=track` on tagged FLAC input (differences only in trailing
silence padding).

## How it hooks into ffplay

`fftools/ffplay.c` is **never edited** (UPSTREAM.toml requires it to stay
byte-identical to the upstream snapshot). `cmake/gain_inject.cmake` rewrites
a *build-tree copy* with three deterministic hooks (symbols per backend,
`vlc` names shown):

1. `#include "ffplay_gain.h"` after `#include "opt_common.h"`.
2. `wanted_spec.callback = ffplay_gain_wrap_callback(sdl_audio_callback);`
   in `audio_open()` — the trampoline runs the original callback, then
   applies the gain to the S16 output (post-mix, so SDL user volume keeps
   its role; mpv backend also honors `FFPLAY_MPV_CAPTURE` here).
3. `ffplay_gain_on_stream(ic, ic->streams[stream_index]);` in
   `stream_component_open()` — recomputes the factor from the new stream's
   replaygain data.

A missed or duplicated anchor fails the configure step, so upstream drift is
caught instead of silently producing a feature-less binary.

## Build & test

```sh
cmake --build build --target test_replay_gain test_mpv_gain
./build/Release/test_replay_gain.exe
./build/Release/test_mpv_gain.exe   # prints an informational mpv-vs-VLC table
```

The cross table quantifies where the two algorithms intentionally differ
(fallback defaults, `REPLAYGAIN_REFERENCE_LOUDNESS` handling, Opus R128
support).

## Notes & caveats

- Gain applies **after** the SDL mix, so amplifying gain can clip twice
  (in `SDL_MixAudioFormat`, then in the kernel). Inaudible for replay gain's
  typical attenuation; otherwise move hook 2 into the `sdl_audio_callback`
  copy loop (pre-mix).
- `REPLAYGAIN_REFERENCE_LOUDNESS`: VLC treats it as LUFS (dB SPL tags like
  `89` shift gain hugely); mpv ignores the tag — each backend mirrors its
  origin.
- Ported files keep provenance headers for upstream diffing. VLC code is
  LGPL-2.1+, mpv code GPLv2+ (0.41.0); keep attribution when distributing —
  linking the mpv backend obliges GPL compliance.
