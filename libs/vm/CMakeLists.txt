add_library(daw_vm STATIC
    src/timeline_view.cpp
)
add_library(daw::vm ALIAS daw_vm)

target_include_directories(daw_vm
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(daw_vm
    PUBLIC
        daw::base
)

if(MSVC)
    target_compile_options(daw_vm PRIVATE /W4 /WX /permissive-)
endif()

if(DAW_BUILD_TESTS)
    add_subdirectory(tests)
endif()
