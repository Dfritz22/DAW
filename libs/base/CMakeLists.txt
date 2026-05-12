# ── libs/base ────────────────────────────────────────────────────────────────
# Bottom-of-the-stack utilities: Result/Expected, asserts, logging primitives,
# small containers. No dependencies on anything else in the project, no Win32,
# no I/O. Header-only today; promoted to a real library once a .cpp lands.
#
# Layer rule: nothing in this library may #include from any other DAW layer.
# Enforced by tools/layer_lint.ps1.

add_library(daw_base INTERFACE)
add_library(daw::base ALIAS daw_base)

target_include_directories(daw_base
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(daw_base INTERFACE cxx_std_20)

if(DAW_BUILD_TESTS)
    add_subdirectory(tests)
endif()
