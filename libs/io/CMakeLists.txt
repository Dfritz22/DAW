# ── libs/io ──────────────────────────────────────────────────────────────────
# Pure file-format codecs with no Win32, no AppState, no DSP. Inputs and
# outputs are POD: paths are wstrings, audio is plain `vector<float>` /
# `vector<uint8_t>`. The codec layer is the boundary the rest of the app
# crosses to read or write user files; AppState adapters live one layer up.
#
# Today this hosts the WAV codec only. Phase 2+ moves project_io and any
# other on-disk formats here as their AppState dependencies are unwound.
#
# Layer rule: may depend on `base` and `platform` only. No core/, no dsp/,
# no AppState.

add_library(daw_io STATIC
    src/wav.cpp
)
add_library(daw::io ALIAS daw_io)

target_include_directories(daw_io
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(daw_io
    PUBLIC
        daw::base
        daw::platform
)

if(MSVC)
    target_compile_options(daw_io PRIVATE /W4 /WX /permissive-)
endif()

if(DAW_BUILD_TESTS)
    add_subdirectory(tests)
endif()
