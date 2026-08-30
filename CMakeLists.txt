# ffplay-standalone — build upstream ffplay.c unmodified against prebuilt
# FFmpeg shared libraries (BtbN builds on Windows and Linux; no FFmpeg
# compilation involved).
#
# UPSTREAM.toml is the single version knob: CMake reads the per-platform
# asset from it; scripts/update.ps1 / update.sh keep the vendored fftools
# snapshot aligned with the same package. See README.md.
cmake_minimum_required(VERSION 3.20)
project(ffplay-standalone C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WIN32)
    set(_platform "windows")
else()
    set(_platform "linux")
endif()

# ---- versions ----
file(STRINGS "${CMAKE_SOURCE_DIR}/UPSTREAM.toml" _toml_lines)
set(_section "")
foreach(_line IN LISTS _toml_lines)
    if(_line MATCHES "^[ \t]*\\[([^]]+)\\]")
        set(_section "${CMAKE_MATCH_1}")
    elseif(_section STREQUAL _platform AND _line MATCHES "^[ \t]*asset[ \t]*=[ \t]*\"([^\"]+)\"")
        set(FFMPEG_ASSET "${CMAKE_MATCH_1}")
    elseif(_section STREQUAL "windows" AND _line MATCHES "^[ \t]*sdl2_version[ \t]*=[ \t]*\"([^\"]+)\"")
        set(SDL2_VERSION "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(NOT FFMPEG_ASSET)
    message(FATAL_ERROR "UPSTREAM.toml: no asset for platform '${_platform}' - run scripts/update.ps1 / update.sh")
endif()

set(THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party" CACHE PATH
    "Directory for downloaded prebuilt trees (gitignored)")
set(FFMPEG_URL "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/${FFMPEG_ASSET}")
string(REGEX REPLACE "\\.(zip|tar\\.xz|tar\\.gz)$" "" _tree_name "${FFMPEG_ASSET}")
set(FFMPEG_PREBUILT "${THIRD_PARTY_DIR}/${_tree_name}")

# ---- fetch prebuilt trees ----
function(ffplay_fetch url marker)
    if(EXISTS "${marker}")
        return()
    endif()
    file(MAKE_DIRECTORY "${THIRD_PARTY_DIR}/.cache")
    get_filename_component(_name "${url}" NAME)
    set(_zip "${THIRD_PARTY_DIR}/.cache/${_name}")
    if(NOT EXISTS "${_zip}")
        message(STATUS "Downloading ${_name}")
        file(DOWNLOAD "${url}" "${_zip}" SHOW_PROGRESS TLS_VERIFY ON STATUS _st)
        list(GET _st 0 _code)
        if(NOT _code EQUAL 0)
            file(REMOVE "${_zip}")
            # file(DOWNLOAD) uses its own TLS stack and fails behind cert-intercepting
            # proxies; fall back to the system curl (schannel trusts the local store).
            set(_curl "$ENV{WINDIR}/System32/curl.exe")
            if(WIN32 AND EXISTS "${_curl}")
                execute_process(COMMAND "${_curl}" -sSL --ssl-no-revoke -o "${_zip}" "${url}"
                    RESULT_VARIABLE _rc ERROR_QUIET)
                if(NOT _rc EQUAL 0)
                    execute_process(COMMAND "${_curl}" -sSL -o "${_zip}" "${url}"
                        RESULT_VARIABLE _rc ERROR_QUIET)
                endif()
            else()
                execute_process(COMMAND curl -sSL -o "${_zip}" "${url}" RESULT_VARIABLE _rc ERROR_QUIET)
            endif()
            if(NOT _rc EQUAL 0)
                list(GET _st 1 _err)
                message(FATAL_ERROR "Download failed: ${_err}\n  ${url}")
            endif()
        endif()
    endif()
    message(STATUS "Extracting ${_name}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xf "${_zip}"
        WORKING_DIRECTORY "${THIRD_PARTY_DIR}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0 OR NOT EXISTS "${marker}")
        message(FATAL_ERROR
            "Extract failed (rc=${_rc}) or tree incomplete: ${marker}\n"
            "If a corrupt/partial archive is cached, delete ${_zip} and reconfigure.")
    endif()
endfunction()

ffplay_fetch("${FFMPEG_URL}" "${FFMPEG_PREBUILT}/include/libavutil/avutil.h")

# ---- generated config.h from prebuilt ----
# fftools needs a config.h. We synthesize one matching the prebuilt package so
# the banner matches and opt_common.c does not print its "library configuration
# mismatch" warning (it compares against the DLLs' embedded string at runtime).
set(GEN_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")

function(ffplay_c_escape out text)
    string(REPLACE "\\" "\\\\" _t "${text}")
    string(REPLACE "\"" "\\\"" _t "${_t}")
    string(REPLACE "\r" "" _t "${_t}")
    string(REPLACE "\n" " " _t "${_t}")
    set("${out}" "${_t}" PARENT_SCOPE)
endfunction()

set(FFMPEG_VERSION_STR "unknown")
set(FFMPEG_CONFIGURATION_ESC "(incomplete prebuilt tree: no bin/ffplay)")
set(CC_IDENT_ESC "unknown")

file(STRINGS "${FFMPEG_PREBUILT}/include/libavutil/ffversion.h" _ver REGEX "define FFMPEG_VERSION")
if(_ver MATCHES "define FFMPEG_VERSION \"([^\"]+)\"")
    set(FFMPEG_VERSION_STR "${CMAKE_MATCH_1}")
endif()

find_program(FFPLAY_REF NAMES ffplay HINTS "${FFMPEG_PREBUILT}/bin" NO_DEFAULT_PATH)
if(NOT FFPLAY_REF)
    message(FATAL_ERROR "prebuilt tree has no bin/ffplay: ${FFMPEG_PREBUILT}")
endif()
# On Linux the package's own binaries have no rpath and need its lib/ on the
# library path to run; Windows DLL resolution finds them next to the exe.
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
    "LD_LIBRARY_PATH=${FFMPEG_PREBUILT}/lib"
    "${FFPLAY_REF}" -hide_banner -version
    OUTPUT_VARIABLE _banner ERROR_VARIABLE _banner_err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
string(REPLACE "\r" "" _banner "${_banner}\n${_banner_err}")
string(REGEX MATCH "configuration: ([^\n]*)" _m "${_banner}")
if(_m)
    ffplay_c_escape(FFMPEG_CONFIGURATION_ESC "${CMAKE_MATCH_1}")
endif()
string(REGEX MATCH "built with ([^\n]*)" _m "${_banner}")
if(_m)
    ffplay_c_escape(CC_IDENT_ESC "${CMAKE_MATCH_1}")
endif()

string(TIMESTAMP CONFIG_THIS_YEAR "%Y")
if(WIN32)
    set(HAVE_SETDLLDIRECTORY 1)
    set(HAVE_COMMANDLINETOARGVW 1)
    set(HAVE_GETMODULEHANDLE 1)
else()
    set(HAVE_SETDLLDIRECTORY 0)
    set(HAVE_COMMANDLINETOARGVW 0)
    set(HAVE_GETMODULEHANDLE 0)
endif()
configure_file("${CMAKE_SOURCE_DIR}/config.h.in" "${GEN_DIR}/config.h" @ONLY)
# ffplay.c includes "config_components.h"; the component macros live in
# config.h, so this shim exists only to satisfy the include.
file(WRITE "${GEN_DIR}/config_components.h"
    "#ifndef FFPLAY_CONFIG_COMPONENTS_SHIM\n"
    "#define FFPLAY_CONFIG_COMPONENTS_SHIM\n"
    "#include \"config.h\"\n"
    "#endif\n")

# ---- target ----
# Snapshot-agnostic source list: older snapshots have no ffplay_renderer.c,
# newer ones do; the vendored tree is always the authority.
file(GLOB FFPLAY_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/fftools/*.c")

# ---- gain experiment ----
# All gain-experiment build logic (hook injection, backends, tests, the
# optional libobs reference dumper) lives in gain/gain.cmake; it patches
# FFPLAY_SOURCES before the target is created. Turning the option off
# (FFPLAY_VLC_GAIN=OFF) restores the plain upstream build.
include("${CMAKE_SOURCE_DIR}/gain/gain.cmake")

add_executable(ffplay ${FFPLAY_SOURCES})

target_include_directories(ffplay PRIVATE
    "${GEN_DIR}"
    "${CMAKE_SOURCE_DIR}/shim"
    "${FFMPEG_PREBUILT}/include")

set(_ffmpeg_libs avutil avcodec avformat avfilter avdevice swscale swresample)
foreach(_lib IN LISTS _ffmpeg_libs)
    if(WIN32)
        set(_libfile "${FFMPEG_PREBUILT}/lib/${_lib}.lib")
    else()
        set(_libfile "${FFMPEG_PREBUILT}/lib/lib${_lib}.so")
    endif()
    if(NOT EXISTS "${_libfile}")
        message(FATAL_ERROR "Import library not found: ${_libfile}")
    endif()
    target_link_libraries(ffplay PRIVATE "${_libfile}")
endforeach()

if(WIN32)
    # SDL2 MSVC dev zip supplies headers + import lib; the runtime DLL also
    # ships in the prebuilt package's bin/, but keep the dev zip authoritative.
    set(SDL2_URL "https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-VC.zip")
    set(SDL2_ROOT "${THIRD_PARTY_DIR}/SDL2-${SDL2_VERSION}")
    ffplay_fetch("${SDL2_URL}" "${SDL2_ROOT}/include/SDL.h")
    target_include_directories(ffplay PRIVATE "${SDL2_ROOT}/include")
    # x64 layout of the SDL2 VC zip; adjust lib/ dir if building another arch.
    target_link_libraries(ffplay PRIVATE "${SDL2_ROOT}/lib/x64/SDL2.lib" shell32 comdlg32)

    if(MSVC)
        target_compile_options(ffplay PRIVATE /utf-8)
    endif()

    # Without the DLLs next to the exe the process exits before main() with no
    # console output at all.
    file(GLOB _prebuilt_dlls "${FFMPEG_PREBUILT}/bin/*.dll")
    add_custom_command(TARGET ffplay POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${SDL2_ROOT}/lib/x64/SDL2.dll" "$<TARGET_FILE_DIR:ffplay>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_prebuilt_dlls} "$<TARGET_FILE_DIR:ffplay>"
        VERBATIM)
else()
    # SDL2 from the system (apt install libsdl2-dev): headers for compiling and
    # the runtime .so come from the distro. m/pthread/dl: modern binutils no
    # longer pull DSOs transitively, and ffplay references libm directly.
    find_package(SDL2 REQUIRED)
    target_link_libraries(ffplay PRIVATE SDL2::SDL2 m pthread dl)

    # Keep the prebuilt package's shared libraries next to the exe so it runs
    # from the build directory without touching LD_LIBRARY_PATH.
    set_target_properties(ffplay PROPERTIES BUILD_RPATH "$ORIGIN")
    file(GLOB _prebuilt_sos "${FFMPEG_PREBUILT}/lib/lib*.so*")
    add_custom_command(TARGET ffplay POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_prebuilt_sos} "$<TARGET_FILE_DIR:ffplay>"
        VERBATIM)
endif()
