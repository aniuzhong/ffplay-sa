# ffplay-standalone — build upstream ffplay.c unmodified against gyan.dev
# prebuilt FFmpeg shared libraries (MSVC, no FFmpeg compilation).
#
# UPSTREAM.toml is the single version knob: CMake reads gyan/sdl2 versions from
# it; scripts/update-gyan.ps1 and scripts/sync-fftools.ps1 keep the vendored
# fftools snapshot aligned with the same package. See README.md.
cmake_minimum_required(VERSION 3.20)
project(ffplay-standalone C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- versions ----
file(STRINGS "${CMAKE_SOURCE_DIR}/UPSTREAM.toml" _toml_lines)
foreach(_line IN LISTS _toml_lines)
    if(_line MATCHES "^[ \t]*version[ \t]*=[ \t]*\"([^\"]+)\"")
        set(_gyan_version "${CMAKE_MATCH_1}")
    elseif(_line MATCHES "^[ \t]*sdl2_version[ \t]*=[ \t]*\"([^\"]+)\"")
        set(_sdl2_version "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(NOT _gyan_version OR NOT _sdl2_version)
    message(FATAL_ERROR "UPSTREAM.toml is missing [gyan] version / sdl2_version — run scripts/update-gyan.ps1")
endif()
set(GYAN_VERSION "${_gyan_version}")
set(SDL2_VERSION "${_sdl2_version}")

set(THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party" CACHE PATH
    "Directory for downloaded prebuilt trees (gitignored)")
set(GYAN_URL "https://github.com/GyanD/codexffmpeg/releases/download/${GYAN_VERSION}/ffmpeg-${GYAN_VERSION}-full_build-shared.zip")
set(SDL2_URL "https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-VC.zip")
set(GYAN_ROOT "${THIRD_PARTY_DIR}/ffmpeg-${GYAN_VERSION}-full_build-shared")
set(SDL2_ROOT "${THIRD_PARTY_DIR}/SDL2-${SDL2_VERSION}")

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
            if(EXISTS "${_curl}")
                execute_process(COMMAND "${_curl}" -sSL --ssl-no-revoke -o "${_zip}" "${url}"
                    RESULT_VARIABLE _rc ERROR_QUIET)
                if(NOT _rc EQUAL 0)
                    execute_process(COMMAND "${_curl}" -sSL -o "${_zip}" "${url}"
                        RESULT_VARIABLE _rc ERROR_QUIET)
                endif()
            else()
                set(_rc 1)
            endif()
            if(NOT _rc EQUAL 0)
                list(GET _st 1 _err)
                message(FATAL_ERROR "Download failed: ${_err}\n  ${url}")
            endif()
        endif()
    endif()
    message(STATUS "Extracting ${_name}")
    # cmake -E tar instead of file(ARCHIVE_EXTRACT): the latter's argument
    # grammar changed across CMake 4.x releases.
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xf "${_zip}"
        WORKING_DIRECTORY "${THIRD_PARTY_DIR}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0 OR NOT EXISTS "${marker}")
        message(FATAL_ERROR
            "Extract failed (rc=${_rc}) or tree incomplete: ${marker}\n"
            "If a corrupt/partial archive is cached, delete ${_zip} and reconfigure.")
    endif()
endfunction()

ffplay_fetch("${GYAN_URL}" "${GYAN_ROOT}/include/libavutil/avutil.h")
ffplay_fetch("${SDL2_URL}" "${SDL2_ROOT}/include/SDL.h")

# ---- generated config.h from gyan tree ----
# fftools needs a config.h. We synthesize one matching the gyan package so the
# banner matches and opt_common.c does not print its "library configuration
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
set(FFMPEG_CONFIGURATION_ESC "(incomplete gyan tree: no bin/ffplay)")
set(CC_IDENT_ESC "unknown")

file(STRINGS "${GYAN_ROOT}/include/libavutil/ffversion.h" _ver REGEX "define FFMPEG_VERSION")
if(_ver MATCHES "define FFMPEG_VERSION \"([^\"]+)\"")
    set(FFMPEG_VERSION_STR "${CMAKE_MATCH_1}")
endif()

find_program(GYAN_FFPLAY_REF NAMES ffplay HINTS "${GYAN_ROOT}/bin" NO_DEFAULT_PATH)
if(NOT GYAN_FFPLAY_REF)
    message(FATAL_ERROR "gyan tree has no bin/ffplay(.exe): ${GYAN_ROOT}")
endif()
execute_process(COMMAND "${GYAN_FFPLAY_REF}" -hide_banner -version
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
add_executable(ffplay ${FFPLAY_SOURCES})

target_include_directories(ffplay PRIVATE
    "${GEN_DIR}"
    "${CMAKE_SOURCE_DIR}/shim"
    "${GYAN_ROOT}/include"
    "${SDL2_ROOT}/include")

set(_ffmpeg_libs avutil avcodec avformat avfilter avdevice swscale swresample)
foreach(_lib IN LISTS _ffmpeg_libs)
    if(NOT EXISTS "${GYAN_ROOT}/lib/${_lib}.lib")
        message(FATAL_ERROR "Import library not found: ${GYAN_ROOT}/lib/${_lib}.lib")
    endif()
    target_link_libraries(ffplay PRIVATE "${GYAN_ROOT}/lib/${_lib}.lib")
endforeach()
# x64 layout of the SDL2 VC zip; adjust lib/ dir if building another arch.
target_link_libraries(ffplay PRIVATE "${SDL2_ROOT}/lib/x64/SDL2.lib" shell32 comdlg32)

if(MSVC)
    target_compile_options(ffplay PRIVATE /utf-8)
endif()

# ---- runtime DLLs ----
# Without the DLLs next to the exe the process exits before main() with no
# console output at all.
file(GLOB _gyan_dlls "${GYAN_ROOT}/bin/*.dll")
add_custom_command(TARGET ffplay POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SDL2_ROOT}/lib/x64/SDL2.dll" "$<TARGET_FILE_DIR:ffplay>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_gyan_dlls} "$<TARGET_FILE_DIR:ffplay>"
    VERBATIM)