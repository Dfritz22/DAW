cmake_minimum_required(VERSION 3.20)
project(DawWorkspace VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Build options ────────────────────────────────────────────────────────────
# DAW_BUILD_TESTS pulls in GoogleTest via FetchContent and registers each
# library's tests/ subdirectory. Default ON so CI and local dev both run
# the test suite by default; flip OFF to skip the GTest fetch on slow links.
option(DAW_BUILD_TESTS "Build unit tests for daw libraries" ON)

if(DAW_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    # Don't install GTest with the project, and force shared CRT on MSVC so
    # GTest links cleanly against our default /MD targets.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST          OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
endif()

# ── Layer libraries (overhaul Phase 0+) ──────────────────────────────────────
# Each layer is its own static (or interface) library with declared deps.
# Upward includes are forbidden; enforced by tools/layer_lint.ps1.
add_subdirectory(libs/base)
add_subdirectory(libs/platform)
add_subdirectory(libs/io)
add_subdirectory(libs/dsp)
add_subdirectory(libs/core)
add_subdirectory(libs/engine)
add_subdirectory(libs/services)
add_subdirectory(libs/vm)

# ── Existing packages / apps ─────────────────────────────────────────────────
add_subdirectory(packages/daw_core)
add_subdirectory(apps/daw_host)
add_subdirectory(apps/daw_gui)

# ── Layer lint convenience target ────────────────────────────────────────────
# Run with: cmake --build build --target layer_lint
if(WIN32)
    add_custom_target(layer_lint
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/tools/layer_lint.ps1"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking layer dependency direction..."
        VERBATIM
    )
endif()
