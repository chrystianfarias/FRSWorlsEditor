cmake_minimum_required(VERSION 3.21)
project(FRSWorldLoader LANGUAGES CXX RC)

# The game is a 32-bit process, so this plugin must be built for Win32 even
# though the editor itself is x64:
#
#   cmake -S loader -B loader/build -G "Visual Studio 17 2022" -A Win32
#   cmake --build loader/build --config Release

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(FRSWorldLoader SHARED
    src/Main.cpp
    src/FileEdits.cpp
    src/Recorder.cpp
    src/Version.rc
)

# Parts of its own, from the same extensions folder as the editor's
# (../private by default); none in this repository's build.
set(FRS_EXTENSIONS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../private" CACHE PATH "Loader extensions, if any")
option(FRS_WITH_EXTENSIONS "Build the extensions in FRS_EXTENSIONS_DIR when it is there" ON)
if(FRS_WITH_EXTENSIONS AND EXISTS "${FRS_EXTENSIONS_DIR}/loader.cmake")
    include("${FRS_EXTENSIONS_DIR}/loader.cmake")
    message(STATUS "FRS World Loader: extensions from ${FRS_EXTENSIONS_DIR}")
else()
    target_sources(FRSWorldLoader PRIVATE src/NoExtensions.cpp)
endif()

target_compile_definitions(FRSWorldLoader PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)

if(MSVC)
    target_compile_options(FRSWorldLoader PRIVATE /W4 /permissive- /utf-8)
    # The C runtime is linked in: the plugin is one file with nothing to
    # install beside it.
    set_property(TARGET FRSWorldLoader PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# The ASI loader looks for .asi, which is just a renamed DLL.
set_target_properties(FRSWorldLoader PROPERTIES
    OUTPUT_NAME "FRSWorldLoader"
    SUFFIX ".asi")
