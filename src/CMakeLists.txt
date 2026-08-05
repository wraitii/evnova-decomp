add_library(evnova_runtime
    brgr_archive.cpp
    game/intro_cinematic.cpp
    game/new_pilot_flow.cpp
    game/pilot_file.cpp
    game/spaceflight.cpp
    game/scenario_data.cpp
    log.cpp
    nova_app.cpp
    pict_image.cpp
    rle_sprite_sheet.cpp
    sdl_audio.cpp
    sdl_music.cpp
    sdl_platform.cpp
)

target_include_directories(evnova_runtime PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(evnova_runtime PUBLIC SDL3::SDL3 SDL3_mixer::SDL3_mixer fmt::fmt)

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
