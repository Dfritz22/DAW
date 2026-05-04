add_library(daw_core
    src/Engine.cpp
)

target_include_directories(daw_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(daw_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(daw_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
