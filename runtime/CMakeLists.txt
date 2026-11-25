cmake_minimum_required(VERSION 3.28)
project(LyraWindowsRuntime LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    message(FATAL_ERROR "Build the Windows application runtime with LLVM-MinGW UCRT.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

include(../cmake/LyraRuntime.cmake)
include(../cmake/LyraSdk.cmake)
lyra_add_runtime()
