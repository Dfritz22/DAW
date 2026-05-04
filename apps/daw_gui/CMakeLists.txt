add_executable(daw_gui WIN32
    main.cpp
    draw.cpp
)

target_link_libraries(daw_gui
    PRIVATE
        daw_core
)

target_compile_definitions(daw_gui PRIVATE UNICODE _UNICODE)
