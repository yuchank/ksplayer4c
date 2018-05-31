cmake_minimum_required(VERSION 3.10)
project(ksplayer4c C)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -D__STDC_CONSTANT_MACROS -w -Wl,-subsystem,console")
set(SDL2_PATH "${CMAKE_SOURCE_DIR}/x86_64-w64-mingw32")
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${ksplayer4c_SOURCE_DIR}/cmake")
find_package(SDL2 REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/include ${SDL2_INCLUDE_DIR})
link_directories(${CMAKE_SOURCE_DIR}/lib)

set(CMAKE_C_STANDARD 99)
set(SOURCE_FILES src/main.c src/cmdutils.c src/cmdutils.h)

add_executable(ksplayer4c ${SOURCE_FILES})
target_link_libraries(ksplayer4c avdevice avformat avutil avcodec swscale swresample ${SDL2_LIBRARY})