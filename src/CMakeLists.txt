add_library(evnova_runtime
    brgr_archive.cpp
    log.cpp
    nova_app.cpp
    pict_image.cpp
    sdl_platform.cpp
)

target_include_directories(evnova_runtime PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(evnova_runtime PUBLIC SDL3::SDL3 fmt::fmt)

target_compile_features(evnova_runtime PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(evnova_runtime PRIVATE /W4 /permissive-)
    if(EVNOVA_WARNINGS_AS_ERRORS)
        target_compile_options(evnova_runtime PRIVATE /WX)
    endif()
else()
    target_compile_options(evnova_runtime PRIVATE -Wall -Wextra -Wpedantic)
    if(EVNOVA_WARNINGS_AS_ERRORS)
        target_compile_options(evnova_runtime PRIVATE -Werror)
    endif()
endif()

add_executable(evnova main.cpp)
target_link_libraries(evnova PRIVATE evnova_runtime)
