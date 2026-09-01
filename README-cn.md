[🇨🇳 简体中文](README-cn.md) | [🇺🇸 English](README.md)

[![License: GPL v3](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)

# ffplay-standalone

基于预编译的 FFmpeg 共享库构建未经修改的上游
[ffplay](https://github.com/FFmpeg/FFmpeg/blob/master/fftools/ffplay.c) ——
无需编译 FFmpeg。两个平台使用同一提供者：**BtbN 的每日 GPL-shared 构建**
（win64 / linux64）；Windows 使用 CMake + MSVC，Linux 使用 CMake + GCC。

`fftools/` 是上游的逐字节快照 —— 请勿手动编辑。所有本地适配都位于
`shim/`（预编译包未安装的头文件）以及一个从该包自身探测生成的
`config.h`，因此 `ffplay -version` 与该包自带的 ffplay 逐字节一致。

## 构建

| | Windows | Linux |
|---|---|---|
| 一次性准备 | VS2022, CMake 3.16.2+, Python 3.8+ | `apt install build-essential cmake ninja-build libsdl2-dev` |

```
py scripts\update.py                                  # Linux: python3 scripts/update.py
cmake -B build -G "Visual Studio 17 2022" -A x64      # Linux: cmake -B build -G Ninja
cmake --build build --config Release                  # Linux: cmake --build build
```

产物：`build/Release/ffplay.exe`（连同 DLL）或 `build/ffplay`（通过
`$ORIGIN` rpath 关联 `.so`）。构建目录与平台绑定 —— 在 Windows 与 WSL
之间共用同一份检出需要各自独立的目录（`build/`、`build-linux/`）。

## 跟随上游

`scripts/update.py`（仅用标准库）刷新预编译包，将从包内 `ffversion.h`
解析出的源码提交写入 `UPSTREAM.toml`，然后据此提交重新同步 `fftools/`
并重新生成 `shim/`。请在两个平台上接连运行 —— BtbN 每天都用同一个提交
构建所有变体，因此两个平台保持在同一提交上，且每个二进制都可以与其所在
包自带的 ffplay 对比（`-version` 必须完全一致）。

偏差（例如从 release 分支提前拉取某个 ffplay.c 修复）记入
`UPSTREAM.toml` 的 `[sync] fftools_ref`，并附上 `reason`。

## 实验分支

功能实验（`experiment/*`）绝不触碰 `fftools/` 或 `shim/`：它们在
configure 阶段向 ffplay.c 的构建树副本注入钩子，并自带源码、目标和
CMake 选项 —— 任何分支仍可构建纯上游二进制。每个实验在自己的子 README
中记录其钩子与注意事项。
