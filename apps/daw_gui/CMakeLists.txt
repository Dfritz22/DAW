add_executable(daw_gui WIN32
    main.cpp
    app_state_api.cpp
    ui/draw.cpp
    ui/panel.cpp
    ui/dock.cpp
    ui/dock_persist.cpp
    io/wav_io.cpp
    io/project_io.cpp
    ai/automix_bridge.cpp
    core/automation.cpp
    ui/layout.cpp
    ui/wndproc/wheel.cpp
    ui/wndproc/timer.cpp
    ui/wndproc/keys.cpp
    ui/wndproc/rbutton.cpp
    ui/wndproc/lbutton.cpp
    ui/wndproc/mousemove.cpp
    core/timeline.cpp
    core/timeline_edit.cpp
    core/internal_app_services.cpp
    engine/EngineFacade.cpp
    audio/engine_utils.cpp
    audio/engine.cpp
    audio/transport_adapter.cpp
    audio/DeviceFacade.cpp
    audio/device_mme.cpp
    audio/device_wasapi.cpp
    audio/device_common.cpp
)

target_link_libraries(daw_gui
    PRIVATE
        daw_core
        daw::base
        daw::platform
        daw::io
        daw::dsp
        daw::core
        daw::engine
        daw::services
        daw::vm
    Winmm
    Comdlg32
    Ole32
    Msimg32
    Shell32
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
