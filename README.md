[🇨🇳 简体中文](README-cn.md) | [🇺🇸 English](README.md)

[![License: GPL v3](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)

# ffplay-standalone

Build upstream [ffplay](https://github.com/FFmpeg/FFmpeg/blob/master/fftools/ffplay.c)
unmodified against prebuilt FFmpeg shared libraries — no FFmpeg compilation.
One provider for both platforms: **BtbN's daily GPL-shared builds** (win64 /
linux64); CMake + MSVC on Windows, CMake + GCC on Linux.

`fftools/` is a byte-identical upstream snapshot — never hand-edit. All local
adaptation lives in `shim/` (headers the prebuilt package does not install)
and a generated `config.h` probed from the package itself, so `ffplay -version`
matches the package's own ffplay byte-for-byte.

## Build

| | Windows | Linux |
|---|---|---|
| once | VS2022, CMake 3.16.2+, Python 3.8+ | `apt install build-essential cmake ninja-build libsdl2-dev` |

```
py scripts\update.py                                  # Linux: python3 scripts/update.py
cmake -B build -G "Visual Studio 17 2022" -A x64      # Linux: cmake -B build -G Ninja
cmake --build build --config Release                  # Linux: cmake --build build
```

Output: `build/Release/ffplay.exe` (+ DLLs) resp. `build/ffplay` (+ `.so` via
`$ORIGIN` rpath). Build dirs are platform-specific — sharing one checkout
between Windows and WSL requires separate dirs (`build/`, `build-linux/`).

## Tracking upstream

`scripts/update.py` (stdlib only) refreshes the prebuilt package, adopts its
source commit (parsed from the package's `ffversion.h`) into `UPSTREAM.toml`,
then re-syncs `fftools/` and regenerates `shim/` from that commit. Run it on
both platforms back-to-back — BtbN builds every variant from the same commit
each day, so both platforms stay on one commit and each binary can be compared
against its package's own ffplay (`-version` must be identical).

Deviations (e.g. pulling an ffplay.c fix from a release branch ahead of the
package) go into `[sync] fftools_ref` with a `reason` in `UPSTREAM.toml`.

## Experiment branches

Feature experiments (`experiment/*`) never touch `fftools/` or `shim/`: they
inject hooks into a build-tree copy of ffplay.c at configure time and carry
their own sources, targets and CMake option — any branch can still build the
plain upstream binary. Each experiment documents its hooks and caveats in its
own sub-README.
