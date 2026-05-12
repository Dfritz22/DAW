# ── libs/platform ────────────────────────────────────────────────────────────
# Thin OS-specific wrappers: DPI helpers, Win32/COM RAII, threading, file
# system, monotonic clock. The only layer (besides shell/) allowed to include
# <windows.h>. Replacing this directory is the strategy for porting to
# another OS.
#
# Layer rule: may depend on `base` only.

add_library(daw_platform STATIC
    src/dpi.cpp
)
add_library(daw::platform ALIAS daw_platform)

target_include_directories(daw_platform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(daw_platform
    PUBLIC
        daw::base
)

if(WIN32)
    # PUBLIC: UNICODE so wide-char Win32 APIs are picked everywhere consumers
    # of platform compile, matching the existing daw_gui convention.
    # PRIVATE: NOMINMAX is needed inside our .cpp to keep <windows.h>'s
    # min/max macros out of std::min/max, but we don't propagate it because
    # other targets already define it themselves and MSVC warns on a
    # redefinition (even with identical replacement text).
    target_compile_definitions(daw_platform PUBLIC  UNICODE _UNICODE)
    target_compile_definitions(daw_platform PRIVATE NOMINMAX)
endif()

if(MSVC)
    target_compile_options(daw_platform PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(daw_platform PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
