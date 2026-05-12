add_library(daw_services STATIC
    src/transport_fsm.cpp
)
add_library(daw::services ALIAS daw_services)

target_include_directories(daw_services
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(daw_services
    PUBLIC
        daw::base
        daw::core
        daw::engine
)

if(MSVC)
    target_compile_options(daw_services PRIVATE /W4 /WX /permissive-)
endif()

if(DAW_BUILD_TESTS)
    add_subdirectory(tests)
endif()
