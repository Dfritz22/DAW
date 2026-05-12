add_library(daw_engine STATIC
    src/clip_reader.cpp
    src/clip_render.cpp
    src/timeline_layout.cpp
)
add_library(daw::engine ALIAS daw_engine)

target_include_directories(daw_engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(daw_engine
    PUBLIC
        daw::base
        daw::core
        daw::dsp
)

if(MSVC)
    target_compile_options(daw_engine PRIVATE /W4 /WX /permissive-)
endif()

if(DAW_BUILD_TESTS)
    add_subdirectory(tests)
endif()
