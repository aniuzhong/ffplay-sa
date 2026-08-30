# Applies the gain experiment hooks to a pristine copy of fftools/ffplay.c.
#
# Included from CMakeLists.txt at configure time with these variables set:
#   GAIN_IN  = path to the pristine fftools/ffplay.c
#   GAIN_OUT = path of the patched copy to write (build tree)
#
# The upstream file is never modified (UPSTREAM.toml requires fftools/ to stay
# byte-identical to the snapshot); hooks are injected into the build-tree copy
# only. Every anchor must match exactly once — a miss is a fatal error, not a
# silent no-op, so upstream drift is caught at configure time instead of
# producing a feature-less binary.
#
# Anchor checks use string(FIND), a literal search: the anchors contain */
# which string(MATCHES) would interpret as regex quantifiers.

if(NOT DEFINED GAIN_IN OR NOT DEFINED GAIN_OUT)
    message(FATAL_ERROR "gain_inject.cmake is included from CMakeLists.txt with GAIN_IN/GAIN_OUT set")
endif()
# Backend-specific hook symbols, selected by FFPLAY_GAIN_BACKEND in
# CMakeLists.txt (vlc: ffplay_gain.h, mpv: ffplay_gain_mpv.h).
if(NOT DEFINED GAIN_HEADER OR NOT DEFINED GAIN_WRAP OR NOT DEFINED GAIN_ON_STREAM)
    message(FATAL_ERROR "gain_inject.cmake requires GAIN_HEADER, GAIN_WRAP and GAIN_ON_STREAM")
endif()

file(READ "${GAIN_IN}" text)

# Idempotency guard: the pristine snapshot must not mention the hooks.
string(FIND "${text}" "${GAIN_WRAP}" _pos)
if(NOT _pos EQUAL -1)
    message(FATAL_ERROR "gain_inject: ${GAIN_IN} already contains gain hooks")
endif()

macro(gain_require_unique_anchor anchor)
    string(FIND "${text}" "${anchor}" _first)
    if(_first EQUAL -1)
        message(FATAL_ERROR "gain_inject: anchor not found: ${anchor}")
    endif()
    math(EXPR _next "${_first} + 1")
    string(SUBSTRING "${text}" "${_next}" -1 _rest)
    string(FIND "${_rest}" "${anchor}" _second)
    if(NOT _second EQUAL -1)
        message(FATAL_ERROR "gain_inject: anchor not unique: ${anchor}")
    endif()
endmacro()

gain_require_unique_anchor("#include \"opt_common.h\"")
string(REPLACE "#include \"opt_common.h\""
    "#include \"opt_common.h\"\n#include \"${GAIN_HEADER}\""
    text "${text}")

gain_require_unique_anchor("wanted_spec.callback = sdl_audio_callback;")
string(REPLACE "wanted_spec.callback = sdl_audio_callback;"
    "wanted_spec.callback = ${GAIN_WRAP}(sdl_audio_callback);"
    text "${text}")

gain_require_unique_anchor("        /* prepare audio output */")
string(REPLACE "        /* prepare audio output */"
    "        ${GAIN_ON_STREAM}(ic, ic->streams[stream_index]);\n        /* prepare audio output */"
    text "${text}")

# Optional fourth hook: placed after audio_hw_params->freq = spec.freq so the
# backend learns the device's ACTUAL rate and channel count - SDL opens the
# device with SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE,
# so spec (not wanted_spec) defines the frame domain gain runs in.
if(DEFINED GAIN_NOTE_DEVICE)
    gain_require_unique_anchor("audio_hw_params->freq = spec.freq;")
    string(REPLACE "audio_hw_params->freq = spec.freq;"
        "audio_hw_params->freq = spec.freq;\n    ${GAIN_NOTE_DEVICE}(spec.freq, spec.channels);"
        text "${text}")
endif()

# Rewrite only when the content changed so the build does not recompile
# ffplay_patched.c on every reconfigure.
set(_gain_need_write 1)
if(EXISTS "${GAIN_OUT}")
    file(READ "${GAIN_OUT}" _gain_prev)
    if(_gain_prev STREQUAL text)
        set(_gain_need_write 0)
    endif()
endif()
if(_gain_need_write)
    file(WRITE "${GAIN_OUT}" "${text}")
    message(STATUS "gain hooks injected into ${GAIN_OUT}")
endif()
