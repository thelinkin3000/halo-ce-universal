# libglslang_probe.so: glslang (the front end, the SPIR-V generator and the
# C interface, glslang/Include/glslang_c_interface.h) in one shared library
# with the C++ runtime inside it, so that libmain.so stays C and loads it
# with dlopen only when the Vulkan probe asks (tools/android_build.py).
#
# cmake -S port/android/probe/glslang -B <build> -DGLSLANG_SOURCE=<glslang>
cmake_minimum_required(VERSION 3.20)
project(glslang_probe C CXX)

set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_JS OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(${GLSLANG_SOURCE} glslang EXCLUDE_FROM_ALL)

add_library(glslang_probe SHARED empty.cpp)
target_link_libraries(glslang_probe PRIVATE
    -Wl,--whole-archive glslang glslang-default-resource-limits -Wl,--no-whole-archive)
# 16 KB pages, as the host library
target_link_options(glslang_probe PRIVATE -Wl,-z,max-page-size=16384 -Wl,--gc-sections -Wl,--strip-all)
