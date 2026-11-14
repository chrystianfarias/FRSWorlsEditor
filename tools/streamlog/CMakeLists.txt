cmake_minimum_required(VERSION 3.21)
project(FRSStreamLog LANGUAGES CXX)

# A 32-bit plugin for the 32-bit game (build.bat does this):
#
#   cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
#   cmake --build build --config Release

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(FRSStreamLog SHARED src/StreamLog.cpp)

target_compile_definitions(FRSStreamLog PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

if(MSVC)
    target_compile_options(FRSStreamLog PRIVATE /W4 /permissive- /utf-8)
    # No CRT DLL to ship next to it.
    set_property(TARGET FRSStreamLog PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

set_target_properties(FRSStreamLog PROPERTIES
    OUTPUT_NAME "FRSStreamLog"
    SUFFIX ".asi")
