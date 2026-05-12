#pragma once

// Minimal assert wrapper. Will grow into a proper logging-aware assert
// (with category, formatted message, and a debugger-break) in a later
// phase. For now it forwards to <cassert> so usage sites are stable.

#include <cassert>

#define DAW_ASSERT(cond) assert(cond)
#define DAW_ASSERT_MSG(cond, msg) assert((cond) && (msg))
