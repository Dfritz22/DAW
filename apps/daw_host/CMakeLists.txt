add_executable(daw_host
    main.cpp
)

target_link_libraries(daw_host
    PRIVATE
        daw_core
)

if(MSVC)
    target_compile_options(daw_host PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(daw_host PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
