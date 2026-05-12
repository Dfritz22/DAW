#pragma once

// ── platform::Dpi ────────────────────────────────────────────────────────────
// HiDPI scaling primitives. UI layout constants are authored at the standard
// 96-DPI baseline ("logical pixels") and scaled to the active monitor's DPI
// at draw / hit-test time via `daw::platform::Dpi(int)`.
//
// The current global is a Phase-0 placeholder; in Phase 7+ this becomes
// per-window state owned by `shell::DockHost` and the global goes away.
// Until then, the host application updates `g_uiDpi` from WM_CREATE /
// WM_DPICHANGED.

namespace daw::platform {

extern int g_uiDpi;

int Dpi(int logical) noexcept;

} // namespace daw::platform
