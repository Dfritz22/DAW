#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

// ── Legacy shim ─────────────────────────────────────────────────────────────
// HiDPI scaling now lives in `libs/platform/include/platform/dpi.h` as part
// of the architectural overhaul (Phase 0). This header is kept temporarily
// as a forwarding shim so the existing call sites in `daw_gui` keep working
// unchanged. It will be deleted in Phase 7 when the UI subsystem is split
// out and every caller switches to `daw::platform::Dpi`.

#include "platform/dpi.h"

inline int& g_uiDpi = daw::platform::g_uiDpi;

inline int Dpi(int v) {
    return daw::platform::Dpi(v);
}
