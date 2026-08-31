# SDL3_ttf links FreeType -> zlib. vcpkg's freetype wrapper does not
# propagate find_dependency(ZLIB), so resolve zlib first so the transitive
# ZLIB::ZLIB imported target exists for SDL3_ttf's config. vcpkg's zlib port
# builds lib as ZLIB::ZLIBSTATIC while the freetype/PNG targets reference the
# find-module name ZLIB::ZLIB, so alias the two.
find_package(ZLIB CONFIG REQUIRED)
if(NOT TARGET ZLIB::ZLIB AND TARGET ZLIB::ZLIBSTATIC)
  add_library(ZLIB::ZLIB INTERFACE IMPORTED)
  set_target_properties(ZLIB::ZLIB PROPERTIES
    INTERFACE_LINK_LIBRARIES ZLIB::ZLIBSTATIC)
endif()
find_package(SDL3_ttf CONFIG REQUIRED)

add_library(evnova_runtime
    brgr_archive.cpp
    cicn_image.cpp
    game/gameplay_interface.cpp
    game/government.cpp
    game/hud_renderer.cpp
    game/hud_overlay.cpp
    game/impact_effects.cpp
    game/intro_cinematic.cpp
    game/landed_window.cpp
    game/landed_store.cpp
    game/mission.cpp
    game/mission_script.cpp
    game/selection_text_dialog.cpp
    game/asteroid.cpp
    game/boarding_plunder.cpp
    game/about_dialog.cpp
    game/collision.cpp
    game/docked_dialog.cpp
    game/negotiation_dialog.cpp
    game/ship_comm_dialog.cpp
    game/new_pilot_flow.cpp
    game/nova_font.cpp
    game/outfit.cpp
    game/pilot_file.cpp
    game/radar_panel.cpp
    game/preferences.cpp
    game/spaceflight.cpp
    game/spaceflight_view.cpp
    game/scenario_data.cpp
    game/services_buttons.cpp
    game/ui_dialog.cpp
    game/ship_visual.cpp
    game/ship_spawn.cpp
    game/ship_ai.cpp
    game/starmap.cpp
    game/sprite_world.cpp
    game/targeting.cpp
    game/travel.cpp
    game/weapon.cpp
    log.cpp
    nova_app.cpp
    pict_image.cpp
    rle_sprite_sheet.cpp
    sdl_audio.cpp
    sdl_music.cpp
    sdl_platform.cpp
)

target_include_directories(evnova_runtime PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(evnova_runtime PUBLIC SDL3::SDL3 SDL3_mixer::SDL3_mixer SDL3_ttf::SDL3_ttf fmt::fmt)

target_compile_features(evnova_runtime PUBLIC cxx_std_23)
if(MSVC)
    target_compile_options(evnova_runtime PRIVATE /W4 /permissive-)
    if(EVNOVA_WARNINGS_AS_ERRORS)
        target_compile_options(evnova_runtime PRIVATE /WX)
    endif()
else()
    target_compile_options(evnova_runtime PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(EVNOVA_WARNINGS_AS_ERRORS)
        target_compile_options(evnova_runtime PRIVATE -Werror)
    endif()
endif()

add_executable(evnova main.cpp)
target_link_libraries(evnova PRIVATE evnova_runtime)
