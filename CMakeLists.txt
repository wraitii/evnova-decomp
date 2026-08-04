cmake_minimum_required(VERSION 3.25)

project(EscapeVelocityNova
    VERSION 0.1.0
    DESCRIPTION "A clean-room recompilation of Escape Velocity Nova"
    LANGUAGES CXX)

option(EVNOVA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Preserve source-level debugging while avoiding the pathologically slow unoptimized builds
# produced by -O0. This applies to the executable and tests on Clang/GCC-family toolchains.
if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
    add_compile_options($<$<CONFIG:Debug>:-Og>)
endif()

find_package(SDL3 CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

add_subdirectory(src)

include(CTest)
if(BUILD_TESTING)
    find_package(Catch2 3 CONFIG REQUIRED)
    add_subdirectory(tests)
endif()
