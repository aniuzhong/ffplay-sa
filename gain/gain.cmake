# gain/ — volume gain experiment CMake (experiment/volume-gain branch).
#
# Everything build-related for the experiment lives here so the root
# CMakeLists.txt stays identical to main: this file patches the ffplay
# source list (build-tree hook injection, cmake/gain_inject.cmake), links
# all gain backends into one binary behind the runtime dispatcher
# (FFPLAY_GAIN_BACKEND env: vlc|mpv|obs), and adds the unit-test targets
# plus the optional libobs reference dumper.
#
# Included from the root CMakeLists.txt right after the FFPLAY_SOURCES glob
# and before add_executable(ffplay); ffplay target properties are attached
# with cmake_language(DEFER ...) because the target does not exist yet.
#
# This file exists only on the experiment/volume-gain branch.

option(FFPLAY_VLC_GAIN "Build ffplay with the gain experiment (gain/)" ON)

if(NOT FFPLAY_VLC_GAIN)
    return()
endif()

set(GAIN_DIR "${CMAKE_SOURCE_DIR}/gain")

# All backends in one binary; the dispatcher (ffplay_gain_dispatch.c)
# selects at runtime. Backends keep their own adapter symbols and static
# state, so they coexist without conflicts.
set(GAIN_HEADER "ffplay_gain_dispatch.h")
set(GAIN_WRAP "ffplay_gain_dispatch_wrap")
set(GAIN_ON_STREAM "ffplay_gain_dispatch_on_stream")
set(GAIN_NOTE_DEVICE "ffplay_gain_dispatch_note_device")
file(GLOB GAIN_SOURCES
    "${GAIN_DIR}/ffplay_gain_dispatch.c"
    "${GAIN_DIR}/ffplay_gain.c"     "${GAIN_DIR}/vlc/*.c"
    "${GAIN_DIR}/ffplay_gain_mpv.c" "${GAIN_DIR}/mpv/*.c"
    "${GAIN_DIR}/ffplay_gain_obs.c" "${GAIN_DIR}/obs/*.c")

# Configure-time injection: rewrites a build-tree copy of the pristine
# snapshot (the injected hooks reference the dispatcher symbols above).
# CMAKE_CONFIGURE_DEPENDS re-runs configure (and thus re-injects) whenever
# upstream sync touches ffplay.c or the inject script changes.
set(PATCHED_FFPLAY "${GEN_DIR}/ffplay_patched.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/fftools/ffplay.c"
    "${CMAKE_SOURCE_DIR}/cmake/gain_inject.cmake")
set(GAIN_IN "${CMAKE_SOURCE_DIR}/fftools/ffplay.c")
set(GAIN_OUT "${PATCHED_FFPLAY}")
include("${CMAKE_SOURCE_DIR}/cmake/gain_inject.cmake")
list(REMOVE_ITEM FFPLAY_SOURCES "${CMAKE_SOURCE_DIR}/fftools/ffplay.c")
list(APPEND FFPLAY_SOURCES "${PATCHED_FFPLAY}" ${GAIN_SOURCES})

# The patched copy lives in the build tree, so fftools/ must be on the
# include path for its sibling headers (cmdutils.h, ...). Deferred until
# the ffplay target exists (created after this file is included).
cmake_language(DEFER
    CALL target_include_directories ffplay PRIVATE
        "${GAIN_DIR}"
        "${CMAKE_SOURCE_DIR}/fftools")

# ----- unit tests ----
# VLC backend: golden vectors for the replay-gain multiplier math.
add_executable(test_replay_gain
    "${GAIN_DIR}/tests/test_replay_gain.c"
    "${GAIN_DIR}/vlc/replay_gain.c")
target_include_directories(test_replay_gain PRIVATE "${GAIN_DIR}")
if(NOT WIN32)
    target_link_libraries(test_replay_gain PRIVATE m)
endif()

# mpv backend: golden vectors for the ported math plus an informational
# mpv-vs-VLC multiplier comparison (pure C, no FFmpeg).
add_executable(test_mpv_gain
    "${GAIN_DIR}/tests/test_mpv_gain.c"
    "${GAIN_DIR}/mpv/mpv_replaygain.c"
    "${GAIN_DIR}/mpv/mpv_kernels.c"
    "${GAIN_DIR}/vlc/replay_gain.c")
target_include_directories(test_mpv_gain PRIVATE "${GAIN_DIR}")
if(NOT WIN32)
    target_link_libraries(test_mpv_gain PRIVATE m)
endif()

# OBS backend: the verbatim-port math (dB conversion, device sink clamp,
# S16<->float bridge) and the audio-action envelope state machine
# (apply_audio_actions port) (pure C, no FFmpeg/SDL).
add_executable(test_obs_gain
    "${GAIN_DIR}/tests/test_obs_gain.c"
    "${GAIN_DIR}/obs/gain_core.c"
    "${GAIN_DIR}/obs/audio_actions.c")
target_include_directories(test_obs_gain PRIVATE "${GAIN_DIR}")
if(NOT WIN32)
    target_link_libraries(test_obs_gain PRIVATE m)
endif()

# ---- libobs reference ----
# Real-libobs reference dumper for the OBS backend: runs the actual libobs
# pipeline (ffmpeg_source media playback -> obs_source_set_volume ->
# obs_add_raw_audio_callback at the device sink) so the port's dumps can be
# compared against a true reference. Linux-only (apt libobs-dev +
# obs-studio for the media-source plugin); silently skipped elsewhere, so
# Windows builds neither compile nor link libobs.
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBOBS QUIET libobs)
        if(LIBOBS_FOUND)
            # Ubuntu's libobs.pc installs headers into <prefix>/include/obs
            # but omits that -I from Cflags; locate obs.h explicitly.
            find_path(LIBOBS_INCLUDE_DIR obs.h HINTS ${LIBOBS_INCLUDE_DIRS}
                PATH_SUFFIXES obs)
            add_executable(obs_ref_dump "${GAIN_DIR}/obs/reference/obs_ref_dump.c")
            target_include_directories(obs_ref_dump PRIVATE ${LIBOBS_INCLUDE_DIR})
            target_link_libraries(obs_ref_dump PRIVATE ${LIBOBS_LIBRARIES} m)
        endif()
    endif()
endif()
