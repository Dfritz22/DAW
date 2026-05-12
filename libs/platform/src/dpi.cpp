#include "platform/dpi.h"

#include <windows.h>

namespace daw::platform {

int g_uiDpi = 96;

int Dpi(int logical) noexcept {
    return ::MulDiv(logical, g_uiDpi, 96);
}

} // namespace daw::platform
