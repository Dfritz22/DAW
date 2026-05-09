add_executable(daw_gui WIN32
    main.cpp
    app_state_api.cpp
    ui/draw.cpp
    io/wav_io.cpp
    io/project_io.cpp
    ai/automix_bridge.cpp
    dsp/chain.cpp
    core/automation.cpp
    ui/layout.cpp
    core/timeline.cpp
    core/timeline_edit.cpp
    core/internal_app_services.cpp
    engine/EngineFacade.cpp
    audio/engine_utils.cpp
    audio/engine.cpp
    audio/DeviceFacade.cpp
    audio/device_mme.cpp
    audio/device_wasapi.cpp
    audio/device_common.cpp
)

target_link_libraries(daw_gui
    PRIVATE
        daw_core
    Winmm
    Comdlg32
    Ole32
)

target_include_directories(daw_gui
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(daw_gui PRIVATE UNICODE _UNICODE)

if(MSVC)
    target_compile_options(daw_gui PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(daw_gui PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
