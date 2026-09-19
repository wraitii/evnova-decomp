add_executable(evnova_tests
    asteroid_scoop_test.cpp
    asteroid_test.cpp
    cargo_test.cpp
    cicn_test.cpp
    collision_test.cpp
    command_input_test.cpp
    contraband_scan_test.cpp
    docked_dialog_test.cpp
    escort_commands_test.cpp
    escort_fleet_test.cpp
    flight_automation_test.cpp
    gameplay_interface_test.cpp
    government_test.cpp
    hud_overlay_test.cpp
    intro_cinematic_test.cpp
    impact_effects_test.cpp
    landed_window_test.cpp
    movement_test.cpp
    mission_test.cpp
    mission_script_test.cpp
    new_pilot_dialog_test.cpp
    nova_command_test.cpp
    nova_font_test.cpp
    outfit_clamp_test.cpp
    outfit_ionization_test.cpp
    pilot_file_test.cpp
    player_info_window_test.cpp
    pixpat_test.cpp
    preferences_test.cpp
    rank_test.cpp
    render_clip_scope_test.cpp
    scenario_data_test.cpp
    services_buttons_test.cpp
    sdl_audio_test.cpp
    ship_ai_test.cpp
    ship_spawn_test.cpp
    ship_visual_test.cpp
    spaceflight_player_state_test.cpp
    smoke_test.cpp
    sprite_test.cpp
    sprite_mask_test.cpp
    travel_test.cpp
    trade_center_test.cpp
    snd_probe_test.cpp
    desc_probe.cpp
    boarding_strings_test.cpp
    weapon_test.cpp
)

target_link_libraries(evnova_tests PRIVATE Catch2::Catch2WithMain evnova_runtime)

target_compile_features(evnova_tests PRIVATE cxx_std_23)

if(EVNOVA_ENABLE_PCH)
    # clang-tidy runs its own frontend and ignores the PCH, so skip it there.
    if(NOT CMAKE_CXX_CLANG_TIDY)
        target_precompile_headers(evnova_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/pch.hpp")
    else()
        message(STATUS "EVNOVA_ENABLE_PCH ignored: clang-tidy is enabled")
    endif()
endif()

if(MSVC)
    target_compile_options(evnova_tests PRIVATE /W4 /permissive-)
    if(EVNOVA_WARNINGS_AS_ERRORS)
        target_compile_options(evnova_tests PRIVATE /WX)
    endif()
else()
    target_compile_options(evnova_tests PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(EVNOVA_WARNINGS_AS_ERRORS)
        target_compile_options(evnova_tests PRIVATE -Werror)
    endif()
endif()

include(Catch)
# Data-dependent tests locate the game archives relative to the repo root;
# run ctest discoveries from there so the archive-gated cases actually run.
catch_discover_tests(evnova_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
