# Port host for microexec's port comparison (tools/microexec_evnova/README.md).
# Built only with -DEVNOVA_MICROEXEC=ON; links the game runtime unchanged.
#
# The field and target tables are generated from the port headers at build
# time (gen_reflection.py over clang's JSON AST), so a new port field or a new
# `@port`-cited function needs no host code.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(GLOB_RECURSE microexec_port_sources CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/src/*.hpp
    ${PROJECT_SOURCE_DIR}/src/*.cpp
)
set(microexec_generated ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(
    OUTPUT ${microexec_generated}/port_reflection.gen.hpp
           ${microexec_generated}/port_reflection.gen.cpp
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/gen_reflection.py
            --compile-commands ${CMAKE_BINARY_DIR}/compile_commands.json
            --src ${PROJECT_SOURCE_DIR}/src
            --out ${microexec_generated}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen_reflection.py ${microexec_port_sources}
    COMMENT "Generating microexec port reflection"
    VERBATIM
)

# Access tracing (trace.cpp): the port code is recompiled with SanitizerCoverage
# load/store tracing into a private copy of the runtime, so evnova_runtime and
# the game build stay untouched. OFF builds the host against evnova_runtime;
# it still reports writes (by comparing memory before and after the call) but
# cannot report reads.
# `func` only selects a coverage type (clang instruments nothing without one);
# no callback is requested for it.
option(EVNOVA_MICROEXEC_TRACE
    "Build the microexec host against a load/store-instrumented copy of the runtime" ON)
set(microexec_port_library evnova_runtime)
if(EVNOVA_MICROEXEC_TRACE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang")
        message(FATAL_ERROR "EVNOVA_MICROEXEC_TRACE needs clang (SanitizerCoverage load/store tracing)")
    endif()
    get_target_property(runtime_sources evnova_runtime SOURCES)
    set(traced_sources)
    foreach(source IN LISTS runtime_sources)
        cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY ${PROJECT_SOURCE_DIR}/src
                   OUTPUT_VARIABLE source_path)
        list(APPEND traced_sources ${source_path})
    endforeach()
    add_library(evnova_microexec_port STATIC ${traced_sources})
    target_include_directories(evnova_microexec_port PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_include_directories(evnova_microexec_port SYSTEM PRIVATE
        ${PROJECT_SOURCE_DIR}/src/third_party)
    get_target_property(runtime_definitions evnova_runtime INTERFACE_COMPILE_DEFINITIONS)
    if(runtime_definitions)
        target_compile_definitions(evnova_microexec_port PUBLIC ${runtime_definitions})
    endif()
    get_target_property(runtime_libraries evnova_runtime LINK_LIBRARIES)
    target_link_libraries(evnova_microexec_port PUBLIC ${runtime_libraries})
    target_compile_features(evnova_microexec_port PUBLIC cxx_std_23)
    target_compile_options(evnova_microexec_port PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -fsanitize-coverage=func,trace-loads,trace-stores)
    if(EVNOVA_WARNINGS_AS_ERRORS)
        target_compile_options(evnova_microexec_port PRIVATE -Werror)
    endif()
    set(microexec_port_library evnova_microexec_port)
    set_source_files_properties(${microexec_generated}/port_reflection.gen.cpp PROPERTIES
        COMPILE_OPTIONS "-fsanitize-coverage=func,trace-loads,trace-stores")
endif()

add_executable(evnova_microexec_host
    main.cpp
    reflect.cpp
    trace.cpp
    ${microexec_generated}/port_reflection.gen.cpp
)
target_include_directories(evnova_microexec_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${microexec_generated}
)
target_link_libraries(evnova_microexec_host PRIVATE ${microexec_port_library})
if(EVNOVA_MICROEXEC_TRACE)
    target_compile_definitions(evnova_microexec_host PRIVATE EVNOVA_MICROEXEC_TRACE=1)
endif()
target_compile_features(evnova_microexec_host PRIVATE cxx_std_23)
if(NOT MSVC)
    target_compile_options(evnova_microexec_host PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(EVNOVA_WARNINGS_AS_ERRORS)
        target_compile_options(evnova_microexec_host PRIVATE -Werror)
    endif()
endif()
