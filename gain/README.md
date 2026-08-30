# gain/ — volume gain algorithms experiment

`experiment/volume-gain` branch only. Ports external players' gain/volume
algorithms onto ffplay for comparison testing: VLC's replay gain (the
original experiment), mpv's AO gain model, and OBS Studio's float-pipeline
gain (volume actions + device-sink clamp). Player plugin machinery
(VLC `module_need` dispatch, mpv option/property plumbing, OBS properties/
signals/filter chains) is intentionally not ported — plain functions and
env vars replace it.

## Layout

| Path | Origin | Role |
|---|---|---|
| `gain.cmake` | new | All experiment CMake (option, hook injection, backends, tests, libobs dumper); the root CMakeLists.txt only includes this file and stays identical to main otherwise |
| `vlc/replay_gain.{h,c}` | VLC `include/vlc_replay_gain.h`, `src/input/replay_gain.c` | Tag model + multiplier calculation (pure functions) |
| `vlc/amplify.{h,c}` | VLC `modules/audio_mixer/{integer,float}.c` | Per-sample kernels (S16/S32/U8/FL32) |
| `vlc/volume.{h,c}` | VLC `src/audio_output/volume.c` | Lock-free gain factor state + kernel dispatch |
| `mpv/mpv_replaygain.{h,c}` | mpv `player/audio.c`, `demux/demux.c` | `audio_get_gain()` composition, replaygain computation, three-generation tag parsing (pure functions) |
| `mpv/mpv_kernels.{h,c}` | mpv `audio/out/ao.c` | Q8.8 fixed-point / float kernels (`process_plane()`, `ao_post_process_data()`) |
| `obs/audio_math.h` | OBS `libobs/media-io/audio-math.h` | `db_to_mul` / `mul_to_db`, copied verbatim (header-only) |
| `obs/gain_core.{h,c}` | OBS `libobs/obs-source.c`, `media-io/audio-io.c`, `plugins/obs-filters/gain-filter.c` | Multiply kernels, device-sink clamp, volume epsilon folding, S16<->float bridge |
| `obs/audio_actions.{h,c}` | OBS `libobs/obs-source.c` | Timestamped volume actions + per-frame step envelope (`apply_audio_actions`) |
| `ffplay_gain.{h,c}` | new | VLC backend adapter: tag harvest, env config, SDL trampoline |
| `ffplay_gain_mpv.{h,c}` | new | mpv backend adapter: side-data/tag harvest, env config, SDL trampoline, capture hook |
| `ffplay_gain_obs.{h,c}` | new | OBS backend adapter: float pipeline hosting, 1024-frame block state machine, event schedule, dump hook |
| `ffplay_gain_dispatch.{h,c}` | new | Runtime backend selector (`FFPLAY_GAIN_BACKEND`); all backends linked into one binary |
| `tests/` | new | `test_replay_gain.c` (VLC math), `test_mpv_gain.c` (mpv math + kernels + mpv-vs-VLC cross table), `test_obs_gain.c` (OBS math + envelope machine), `verify_dump.py` (bit-exact dump verifier) |

## Backends

All backends link into a single ffplay binary behind the dispatcher;
`FFPLAY_GAIN_BACKEND` (environment, default `vlc`) selects per run:

```sh
FFPLAY_GAIN_BACKEND=obs ffplay file.wav
```

With no env vars set every backend is a no-op (unity gain).

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

### OBS backend (`FFPLAY_GAIN_DB` / `_EVENTS` / `_DUMP`)

OBS models volume as a float-pipeline process on a source: the factor is a
linear multiplier converted from dB, volume changes are timestamped actions
applied by the audio thread as sample-aligned steps, and the mix is clamped
exactly once at the device sink.

| Variable | Default | Meaning (OBS equivalent) |
|---|---|---|
| `FFPLAY_GAIN_DB` | `0` | source volume in dB (`obs_source_set_volume`; the gain filter's UI range is -30..+30 dB) |
| `FFPLAY_GAIN_EVENTS` | unset | `sec:db,sec:db,...` volume steps scheduled at device frame `round(sec*freq)` (OBS `AUDIO_ACTION_VOL` actions) |
| `FFPLAY_GAIN_DUMP` | unset | dump the post-pipeline S16 stream for `verify_dump.py` |

Example: `FFPLAY_GAIN_BACKEND=obs FFPLAY_GAIN_EVENTS="2.0:-6,5.0:0" ffplay file.wav`

### OBS port fidelity

Copied verbatim from OBS Studio: `db_to_mul`/`mul_to_db` (20*log10
convention, -inf dB -> factor 0), the gain-filter multiply kernel, the
`clamp_audio_output` sink rules (NaN -> 0, saturation to +/-1), and the
`apply_audio_actions` envelope loop step for step — including clamping
late actions to the block start, the index-order break that holds
out-of-block actions back, and the `close_float(x, 1.0/0.0, 1e-4)` epsilon
folding of `get_source_volume` (raw storage, folded on read). Volume steps
are sample-aligned **step changes**; OBS interpolates nothing between the
old and new factor.

Re-hosted without numerical effect: ns timestamps clamped against
`audio_ts` -> absolute device-frame indices (the SDL callback carries no
timestamps; the event clock is the frame counter, which is more
deterministic than OBS's wall clock); planar `audio_output_buf` ->
interleaved fan-out of the per-frame factor; OBS's fixed
`AUDIO_OUTPUT_FRAMES` = 1024-frame render blocks -> the trampoline renders
every callback in 1024-frame sub-blocks (a partial tail sub-block only
shifts the render window — step placement is frame-indexed and unaffected).
The mute / push-to-talk state machine inside `get_source_volume` is not
ported (ffplay has no per-source mute UI).

The one adaptation OBS does not have is the S16 <-> float bridge (its
pipeline is float end to end): `s16 / 32768.0f` in, `roundf(f * 32768)`
with saturation out — powers-of-two scaling round-trips exactly, the sink
clamp runs before the conversion, and the same bridge lives in
`verify_dump.py`, so reference and pipeline share one mapping.

**Verified bit-exact** end to end: for a 48 kHz stereo sine WAV, the
`FFPLAY_GAIN_DUMP` capture matches a pure-Python recomputation of the
ported math on every compared sample (765952/765952) for -6 dB, +6 dB
(exercising the 32767 saturation path) and scheduled events
(`"2.0:-6,5.0:0"` — steps land exactly on frames 96000 and 240000).
Cross-backend at -6 dB, the mpv and OBS dumps agree on 90.7% of samples;
the residual deltas (up to 20 LSB at signal peaks) trace to the different
gain entry points (OBS `10^(-6/20) = 0.50119` vs the mpv path's ~0.500
effective factor), not to the kernels. The VLC backend has no dump hook;
use `FFPLAY_GAIN=<linear multiplier>` with a player-side capture if needed.

### Real-libobs cross-check (Linux + WSL2)

`obs/reference/obs_ref_dump.c` runs the **actual libobs pipeline** (apt
`libobs-dev` + `obs-studio` 32.1.0): the stock `ffmpeg_source` media source
plays the test WAV, `obs_source_set_volume()` applies the gain, and
`obs_add_raw_audio_callback()` captures the post-mix float at the device
sink — the same semantic point the port re-hosts. The audio path between
32.1.0 and the ported 32.2.1 tree is identical (verified by diff: only a
monitoring-hotkey refactor and a whitespace change touch these files).

Results, against the same S16-quantized WAV content:

| Scenario | Result |
|---|---|
| -6 dB static | **38048/38048 frames bit-exact** (float32) |
| +6 dB static | **36000/36000 frames bit-exact** with the recovered gain; `powf` differs by 1 ULP from Python's double pow (platform libm, not a port issue) |
| Events `2.0:-6,5.0:0` | Steps land at frames 95998/239998 (**-0.04 ms** vs schedule), sample-aligned step changes, no ramps |

Setup notes for the headless-WSL run (all reproduced in the program):
`obs_reset_video` is required even for audio-only use (the mixer walks
`obs->video.mixes` to build its source list); sources must be attached to a
canvas channel via `obs_set_output_source()`; `source->audio_ts` is
stamped with the wall clock at feed time and re-anchored by
`discard_audio` each tick, so only the official media-source feed path is
reliable for reference captures.

```sh
cmake --build build-linux --target obs_ref_dump
python3 gain/tests/verify_dump.py make build/test_gain.wav
./build-linux/obs_ref_dump -6 build-linux/obs_ref_db6.raw build/test_gain.wav
./build-linux/obs_ref_dump 0 build-linux/obs_ref_events.raw build/test_gain.wav '2.0:-6,5.0:0'
```

## How it hooks into ffplay

`fftools/ffplay.c` is **never edited** (UPSTREAM.toml requires it to stay
byte-identical to the upstream snapshot). `cmake/gain_inject.cmake` rewrites
a *build-tree copy* with four deterministic hooks (dispatch symbols shown):

1. `#include "ffplay_gain_dispatch.h"` after `#include "opt_common.h"`.
2. `wanted_spec.callback = ffplay_gain_dispatch_wrap(sdl_audio_callback);`
   in `audio_open()` — the dispatcher's trampoline runs the original
   callback, then the selected backend applies its gain to the S16 output
   (post-mix, so SDL user volume keeps its role; the mpv backend honors
   `FFPLAY_MPV_CAPTURE` and the OBS backend `FFPLAY_GAIN_DUMP` here).
3. `ffplay_gain_dispatch_on_stream(ic, ic->streams[stream_index]);` in
   `stream_component_open()` — resets backend state per stream (replaygain
   recompute, OBS frame-counter/event-queue reset).
4. `ffplay_gain_dispatch_note_device(spec.freq, spec.channels);` after
   `audio_hw_params->freq = spec.freq;` — SDL negotiates rate and channels
   (`SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE`),
   and the OBS backend's frame domain is the negotiated device rate.

A missed or duplicated anchor fails the configure step, so upstream drift is
caught instead of silently producing a feature-less binary.

## Build & test

```sh
cmake --build build --target test_replay_gain test_mpv_gain test_obs_gain
./build/Release/test_replay_gain.exe
./build/Release/test_mpv_gain.exe   # prints an informational mpv-vs-VLC table
./build/Release/test_obs_gain.exe   # dB round trips, clamp rules, envelope machine
```

The cross table quantifies where the two algorithms intentionally differ
(fallback defaults, `REPLAYGAIN_REFERENCE_LOUDNESS` handling, Opus R128
support).

End-to-end dump verification (OBS backend; bit-exact against a recomputed
reference):

```sh
python gain/tests/verify_dump.py make build/test_gain.wav
FFPLAY_GAIN_BACKEND=obs FFPLAY_GAIN_DB=-6 FFPLAY_GAIN_DUMP=build/obs_db6.raw \
  ./build/Release/ffplay.exe -nodisp -autoexit build/test_gain.wav
python gain/tests/verify_dump.py check build/test_gain.wav build/obs_db6.raw --db -6
```

`check --events "2.0:-6,5.0:0"` verifies the step placement on the same
run configuration used to produce the dump.

## Notes & caveats

- Gain applies **after** the SDL mix, so amplifying gain can clip twice
  (in `SDL_MixAudioFormat`, then in the kernel). Inaudible for replay gain's
  typical attenuation; otherwise move hook 2 into the `sdl_audio_callback`
  copy loop (pre-mix).
- `REPLAYGAIN_REFERENCE_LOUDNESS`: VLC treats it as LUFS (dB SPL tags like
  `89` shift gain hugely); mpv ignores the tag — each backend mirrors its
  origin.
- Ported files keep provenance headers for upstream diffing. VLC code is
  LGPL-2.1+, mpv code GPLv2+ (0.41.0), OBS code GPLv2+ — keep attribution
  when distributing; the mpv and OBS backends oblige GPL compliance.
- OBS specifics: the gain filter's UI range is -30..+30 dB (not enforced
  here — `FFPLAY_GAIN_DB` accepts anything `strtod` does, including
  `-inf` for silence, mirroring `db_to_mul`'s non-finite handling); the
  event schedule is relative to each stream open, and `-autoexit` leaves a
  ~one-callback tail gap in dumps (the verifier compares the overlap).
