cmake_minimum_required(VERSION 3.10)

project(ffplay-standalone C CXX)

find_package(PkgConfig REQUIRED)

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libavdevice
    libavfilter
    libswscale
    libswresample
    libpostproc
)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

add_executable(${PROJECT_NAME}
    ffplay.cpp
    cmdutils.c
    opt_common.c
)
target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(${PROJECT_NAME} PRIVATE "${PROJECT_SOURCE_DIR}/ffmpeg-internal")
target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::FFMPEG)
target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::SDL2)
target_link_libraries(${PROJECT_NAME} PRIVATE m)
