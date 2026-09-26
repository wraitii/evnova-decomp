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
    game/locate_data_dialog.cpp
    game/mission.cpp
    game/mission_script.cpp
    game/mission_text.cpp
    game/mission_trace.cpp
    game/mission_world.cpp
    game/probe_state.cpp
    game/selection_text_dialog.cpp
    game/asteroid.cpp
    game/boarding_plunder.cpp
    game/about_dialog.cpp
    game/collision.cpp
    game/command_input.cpp
    game/docked_bar_dialog.cpp
    game/docked_dialog.cpp
    game/docked_mission_dialog.cpp
    game/docked_store_dialog.cpp
    game/docked_trade_dialog.cpp
    game/escort_commands.cpp
    game/escort_formation.cpp
    game/preferences_extra.cpp
    game/freeflight_objects.cpp
    game/flight_automation.cpp
    game/negotiation_dialog.cpp
    game/ship_comm_dialog.cpp
    game/new_pilot_flow.cpp
    game/nova_font.cpp
    game/nova_list_control.cpp
    game/outfit.cpp
    game/pilot_file.cpp
    game/player_info_window.cpp
    game/radar_panel.cpp
    game/preferences.cpp
    game/rank.cpp
    game/route_map.cpp
    game/spaceflight.cpp
    game/spaceflight_movement.cpp
    game/spaceflight_player_state.cpp
    game/spaceflight_view.cpp
    game/scenario_data.cpp
    game/services_buttons.cpp
    game/ui_dialog.cpp
    game/ship_visual.cpp
    game/ship_spawn.cpp
    game/ship_ai.cpp
    game/ship_ai_behaviors.cpp
    game/ship_ai_controls.cpp
    game/ship_ai_state.cpp
    game/ship_ai_weapons.cpp
    game/starmap.cpp
    game/starmap_render.cpp
    game/trade_center.cpp
    game/sprite_world.cpp
    game/sprite_mask.cpp
    game/targeting.cpp
    game/travel.cpp
    game/weapon.cpp
    game/weapon_banks.cpp
    game/weapon_shots.cpp
    game/world_wrap.cpp
    log.cpp
    nova_app.cpp
    nova_paths.cpp
    pict_image.cpp
    pixpat_image.cpp
    rle_sprite_sheet.cpp
    sdl_audio.cpp
    sdl_music.cpp
    sdl_platform.cpp
)

target_include_directories(evnova_runtime PUBLIC ${PROJECT_SOURCE_DIR}/src)
# Vendored single-header third-party code (dr_mp3); SYSTEM keeps its warnings
# from tripping -Werror on our translation units.
target_include_directories(evnova_runtime PRIVATE SYSTEM
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(evnova_runtime PUBLIC SDL3::SDL3 SDL3_ttf::SDL3_ttf fmt::fmt)

# The probe's POSIX socket server is the only non-portable runtime piece;
# compile the no-op transport when it is disabled so the game-side call sites
# stay compiled and inert (see docs/probe_harness.md).
if(EVNOVA_ENABLE_PROBE)
    target_sources(evnova_runtime PRIVATE probe_server.cpp)
    target_compile_definitions(evnova_runtime PUBLIC EVNOVA_ENABLE_PROBE=1)
else()
    target_sources(evnova_runtime PRIVATE probe_server_stub.cpp)
    target_compile_definitions(evnova_runtime PUBLIC EVNOVA_ENABLE_PROBE=0)
    message(STATUS "Probe harness disabled (EVNOVA_ENABLE_PROBE=OFF)")
endif()

target_compile_features(evnova_runtime PUBLIC cxx_std_23)
if(MSVC)
    # Original fixed-width game fields intentionally narrow arithmetic values.
    target_compile_options(evnova_runtime PRIVATE /W4 /permissive- /wd4244 /wd4267)
    target_compile_definitions(evnova_runtime PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(EVNOVA_WARNINGS_AS_ERRORS)
        target_compile_options(evnova_runtime PRIVATE /WX)
    endif()
else()
    target_compile_options(evnova_runtime PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(EVNOVA_WARNINGS_AS_ERRORS)
        target_compile_options(evnova_runtime PRIVATE -Werror)
    endif()
endif()

if(EVNOVA_ENABLE_PCH)
    # clang-tidy runs its own frontend and ignores the PCH, so skip it there.
    if(NOT CMAKE_CXX_CLANG_TIDY)
        target_precompile_headers(evnova_runtime PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/nova_pch.hpp")
    else()
        message(STATUS "EVNOVA_ENABLE_PCH ignored: clang-tidy is enabled")
    endif()
endif()

add_executable(evnova main.cpp)
target_link_libraries(evnova PRIVATE evnova_runtime)
if(MSVC)
    target_compile_options(evnova PRIVATE /wd4244 /wd4267)
endif()
