# ── libs/dsp ─────────────────────────────────────────────────────────────────
# Pure DSP processors. No Win32, no AppState, no audio device, no I/O.
# Inputs and outputs are POD: interleaved `vector<float>` buffers and
# plain config/state structs. Every processor's persistent state is owned
# by the caller, so the same code paths run identically in offline mixdown
# and the realtime callback.
#
# Layer rule: may depend on `base` and `platform` only. No io/, no core/,
# no engine/, no AppState. The realtime audio thread runs this code, so
# nothing in here may allocate inside the inner sample loop or take locks.

add_library(daw_dsp STATIC
    src/chain.cpp
    src/util.cpp
    src/resample.cpp
    src/mix.cpp
    src/metronome.cpp
)
add_library(daw::dsp ALIAS daw_dsp)

target_include_directories(daw_dsp
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(daw_dsp
    PUBLIC
        daw::base
        daw::platform
)

if(MSVC)
    target_compile_options(daw_dsp PRIVATE /W4 /WX /permissive-)
endif()

if(DAW_BUILD_TESTS)
    add_subdirectory(tests)
endif()
