add_executable(daw_gui WIN32
    main.cpp
    ui/draw.cpp
    io/wav_io.cpp
    io/project_io.cpp
    ai/automix_bridge.cpp
    dsp/chain.cpp
    core/automation.cpp
    ui/layout.cpp
    core/timeline.cpp
    core/timeline_edit.cpp
    audio/engine_utils.cpp
    audio/engine.cpp
    audio/device_mme.cpp
    audio/device_wasapi.cpp
    audio/device_common.cpp
)

target_link_libraries(daw_gui
    PRIVATE
        daw_core
)

target_include_directories(daw_gui
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(daw_gui PRIVATE UNICODE _UNICODE)
