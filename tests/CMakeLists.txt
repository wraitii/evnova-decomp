add_executable(evnova_tests
    game_state_test.cpp
    landed_window_test.cpp
    movement_test.cpp
    outfit_test.cpp
    scenario_data_test.cpp
    smoke_test.cpp
    targeting_test.cpp
    travel_test.cpp
)

target_link_libraries(evnova_tests PRIVATE Catch2::Catch2WithMain evnova_runtime)

target_compile_features(evnova_tests PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(evnova_tests PRIVATE /W4 /permissive-)
    if(EVNOVA_WARNINGS_AS_ERRORS)
        target_compile_options(evnova_tests PRIVATE /WX)
    endif()
else()
    target_compile_options(evnova_tests PRIVATE -Wall -Wextra -Wpedantic)
    if(EVNOVA_WARNINGS_AS_ERRORS)
        target_compile_options(evnova_tests PRIVATE -Werror)
    endif()
endif()

include(Catch)
catch_discover_tests(evnova_tests)
