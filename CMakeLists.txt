cmake_minimum_required(VERSION 3.25)

project(EscapeVelocityNova
    VERSION 0.1.0
    DESCRIPTION "A clean-room recompilation of Escape Velocity Nova"
    LANGUAGES CXX)

option(EVNOVA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SDL3 CONFIG REQUIRED)

add_subdirectory(src)

include(CTest)
if(BUILD_TESTING)
    find_package(Catch2 3 CONFIG REQUIRED)
    add_subdirectory(tests)
endif()
