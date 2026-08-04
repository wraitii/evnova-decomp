add_executable(evnova
    main.cpp
)

target_link_libraries(evnova PRIVATE SDL3::SDL3)

target_compile_features(evnova PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(evnova PRIVATE /W4 /permissive-)
    if(EVNOVA_WARNINGS_AS_ERRORS)
        target_compile_options(evnova PRIVATE /WX)
    endif()
else()
    target_compile_options(evnova PRIVATE -Wall -Wextra -Wpedantic)
    if(EVNOVA_WARNINGS_AS_ERRORS)
        target_compile_options(evnova PRIVATE -Werror)
    endif()
endif()
