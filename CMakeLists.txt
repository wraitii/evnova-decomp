cmake_minimum_required(VERSION 3.25)

project(EscapeVelocityNova
    VERSION 0.1.0
    DESCRIPTION "A clean-room recompilation of Escape Velocity Nova"
    LANGUAGES CXX)

option(EVNOVA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(EVNOVA_ENABLE_CLANG_TIDY "Run clang-tidy during compilation when available" ON)

# External probe/control harness (EVN_PROBE=1, docs/probe_harness.md). Its
# transport is a raw POSIX socket server, so it compiles everywhere except
# Windows (Winsock backend not written yet); there it defaults off so a plain
# `cmake --preset release` still builds.
if(WIN32)
    option(EVNOVA_ENABLE_PROBE "Build the external probe/control harness" OFF)
else()
    option(EVNOVA_ENABLE_PROBE "Build the external probe/control harness" ON)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(EVNOVA_ENABLE_CLANG_TIDY)
    find_program(EVNOVA_CLANG_TIDY NAMES clang-tidy
        HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
    if(EVNOVA_CLANG_TIDY)
        # The check set and WarningsAsErrors live in the repo-root .clang-tidy,
        # so in-build and manual runs stay in sync. Only the sysroot, which
        # clang-tidy cannot infer from compile_commands.json, is passed here.
        set(_evnova_clang_tidy_args)
        if(APPLE)
            # Homebrew LLVM's clang-tidy does not inherit AppleClang's default
            # sysroot, and compile_commands.json has no -isysroot because Apple's
            # clang discovers it implicitly. Pass the SDK explicitly so headers
            # such as AvailabilityMacros.h are found.
            execute_process(COMMAND xcrun --show-sdk-path
                OUTPUT_VARIABLE _evnova_sdk_path
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(_evnova_sdk_path)
                list(APPEND _evnova_clang_tidy_args
                    "--extra-arg=-isysroot" "--extra-arg=${_evnova_sdk_path}")
            endif()
        endif()
        set(CMAKE_CXX_CLANG_TIDY "${EVNOVA_CLANG_TIDY}" ${_evnova_clang_tidy_args})
    else()
        message(STATUS "clang-tidy not found; static analysis is disabled")
    endif()
endif()

# Preserve source-level debugging while avoiding the pathologically slow unoptimized builds
# produced by -O0. This applies to the executable and tests on Clang/GCC-family toolchains.
if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
    add_compile_options($<$<CONFIG:Debug>:-Og>)
endif()

find_package(SDL3 CONFIG REQUIRED)
find_package(SDL3_mixer CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

add_subdirectory(src)

include(CTest)
if(BUILD_TESTING)
    find_package(Catch2 3 CONFIG REQUIRED)
    add_subdirectory(tests)
endif()
