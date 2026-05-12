# ── libs/core ────────────────────────────────────────────────────────────────
# Pure value types and pure evaluators that make up the project's data model.
# No Win32, no AppState, no I/O, no DSP processors. Everything in here is a
# POD struct or a pure function over POD inputs — safe to call from any
# layer above (engine, services, ui) and easy to unit-test in isolation.
#
# Phase 4 ships only the automation-curve types and evaluator. Future phases
# will pull in ProjectData, the timeline math helpers, and similar pure
# pieces as their AppState entanglements are unwound.
#
# Layer rule: may depend on `base`, `platform`, `io`, and `dsp`. No engine/,
# no AppState. Realtime-safe code paths must not allocate or take locks.
#
# Naming note: target is `daw_corelib` (alias `daw::core`) to avoid colliding
# with the existing `daw_core` SDK package in packages/daw_core/. The two
# will be reconciled when the engine layer is itself extracted.

add_library(daw_corelib STATIC
    src/automation_curve.cpp
    src/timeline_math.cpp
    src/timeline_edit.cpp
    src/track_audibility.cpp
)
add_library(daw::core ALIAS daw_corelib)

target_include_directories(daw_corelib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(daw_corelib
    PUBLIC
        daw::base
        daw::platform
        daw::dsp
)

if(MSVC)
    target_compile_options(daw_corelib PRIVATE /W4 /WX /permissive-)
endif()

if(DAW_BUILD_TESTS)
    add_subdirectory(tests)
endif()
