# gain/ — volume gain algorithms experiment

`experiment/volume-gain` branch only. Ports external players' gain/volume
algorithms onto ffplay for testing, starting with VLC's replay gain and
software amplification.

## Layout

| Path | Origin | Role |
|---|---|---|
| `vlc/replay_gain.{h,c}` | VLC `include/vlc_replay_gain.h`, `src/input/replay_gain.c` | Tag model + multiplier calculation (pure functions, unit tested) |
| `vlc/amplify.{h,c}` | VLC `modules/audio_mixer/{integer,float}.c` | Per-sample scaling kernels (S16/S32/U8/FL32) |
| `vlc/volume.{h,c}` | VLC `src/audio_output/volume.c` | Lock-free gain factor state + kernel dispatch |
| `ffplay_gain.{h,c}` | new | AVDictionary tag adapter, env config, SDL callback trampoline |
| `tests/` | new | Unit tests for the multiplier math |

VLC's plugin system (`module_need`, capability dispatch) is intentionally not
ported: a plain function pointer replaces the "audio volume" module lookup.

## How it hooks into ffplay

`fftools/ffplay.c` is **never edited** (UPSTREAM.toml requires it to stay
byte-identical to the upstream snapshot). Instead, `cmake/gain_inject.cmake`
(included from CMakeLists.txt at configure time) rewrites a *build-tree copy*
with three deterministic hooks:

1. `#include "ffplay_gain.h"` after `#include "opt_common.h"`.
2. `wanted_spec.callback = ffplay_gain_wrap_callback(sdl_audio_callback);` in
   `audio_open()` — the trampoline runs the original callback, then applies
   the gain factor to the S16 output stream (post-mix, so the SDL user volume
   in `SDL_MixAudioFormat` keeps its role).
3. `ffplay_gain_on_stream(ic, ic->streams[stream_index]);` at the top of the
   audio case in `stream_component_open()` — recomputes the factor from the
   new stream's REPLAYGAIN tags.

If an anchor is not found (upstream drift), configure fails with a fatal
error instead of silently producing a feature-less binary.

## Configuration (environment variables)

| Variable | Default | Meaning |
|---|---|---|
| `FFPLAY_GAIN_MODE` | `none` | `track`, `album` or `none` (VLC `audio-replay-gain-mode`) |
| `FFPLAY_GAIN_PREAMP` | `0` | pre-amplification in dB (`audio-replay-gain-preamp`) |
| `FFPLAY_GAIN_DEFAULT` | `-7` | dB for files without tags (`audio-replay-gain-default`) |
| `FFPLAY_GAIN_PEAK_PROTECTION` | `1` | clamp multiplier to `1/peak` to avoid clipping |
| `FFPLAY_GAIN` | `1.0` | extra linear multiplier (VLC `gain`) |

With no variables set the feature is a no-op (mode `none`, unity gain).

Example: `FFPLAY_GAIN_MODE=track ffplay file.flac`

## Build & test

The option `FFPLAY_VLC_GAIN` (default `ON` on this branch) enables the hook
injection and the `test_replay_gain` unit test target:

```sh
cmake --build build --target test_replay_gain && ./build/test_replay_gain
```

## Notes & caveats

- Gain is applied **after** the SDL mix, so an amplifying gain can clip twice
  (once in `SDL_MixAudioFormat`, once in the kernel). For replay gain's
  typical attenuation this is inaudible; if it ever matters, move hook 2 into
  the copy loop of `sdl_audio_callback` (pre-mix semantics).
- `REPLAYGAIN_REFERENCE_LOUDNESS` is interpreted as LUFS, like VLC does; tags
  stored as dB SPL (e.g. `89`) shift the gain by a large negative delta.
- Opus `R128_TRACK_GAIN` is a different standard (Q7.8, -20 LUFS reference)
  and is not handled.
- Ported files keep a provenance header referencing the VLC source file and
  function names, to make future diffs against VLC upstream trivial. The VLC
  code is LGPL-2.1+; keep the attribution headers when distributing.
