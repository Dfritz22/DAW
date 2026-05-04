add_executable(daw_host
    main.cpp
)

target_link_libraries(daw_host
    PRIVATE
        daw_core
)
