add_executable(evnova_tests
    asteroid_test.cpp
    cicn_test.cpp
    collision_test.cpp
    gameplay_interface_test.cpp
    government_test.cpp
    hud_overlay_test.cpp
    landed_window_test.cpp
    movement_test.cpp
    scenario_data_test.cpp
    ship_ai_test.cpp
    ship_spawn_test.cpp
    smoke_test.cpp
    sprite_test.cpp
    travel_test.cpp
    snd_probe_test.cpp
    weapon_test.cpp
)

target_link_libraries(evnova_tests PRIVATE Catch2::Catch2WithMain evnova_runtime)

target_compile_features(evnova_tests PRIVATE cxx_std_23)

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
