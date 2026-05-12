#include "core/internal_app_services.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace daw::internal::core {

InsertEffectArray DefaultInsertEffects() {
    InsertEffectArray effects{};
    for (int i = 0; i < kMaxInsertSlots; ++i) {
        effects[static_cast<size_t>(i)] = static_cast<std::uint8_t>(i % kInsertEffectTypeCount);
    }
    return effects;
}

InsertBypassArray DefaultInsertBypass() {
    InsertBypassArray bypass{};
    for (int i = 0; i < kMaxInsertSlots; ++i) {
        bypass[static_cast<size_t>(i)] = false;
    }
    return bypass;
}

InsertConfigArray DefaultInsertConfig() {
    InsertConfigArray arr;
    return arr;
}

std::wstring QuoteArg(const std::wstring& s) {
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        if (c == L'\"') {
            out += L"\\\"";
        } else {
            out += c;
        }
    }
    out += L"\"";
    return out;
}

std::filesystem::path FindRepoRoot() {
    auto hasRepoMarkers = [](const std::filesystem::path& p) {
        return std::filesystem::exists(p / L".venv" / L"Scripts" / L"python.exe") &&
               std::filesystem::exists(p / L"src" / L"daw_ai");
    };

    auto scanUp = [&](std::filesystem::path p) -> std::filesystem::path {
        while (!p.empty()) {
            if (hasRepoMarkers(p)) {
                return p;
            }
            const std::filesystem::path parent = p.parent_path();
            if (parent == p) {
                break;
            }
            p = parent;
        }
        return {};
    };

    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec) {
        std::filesystem::path fromCwd = scanUp(cwd);
        if (!fromCwd.empty()) {
            return fromCwd;
        }
    }

#ifdef _WIN32
    // Fall back to scanning up from the executable's own directory so the app
    // can locate the repo even when launched with an unrelated working dir.
    wchar_t exeBuf[MAX_PATH] = {0};
    const DWORD len = GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::filesystem::path exePath(exeBuf);
        std::filesystem::path fromExe = scanUp(exePath.parent_path());
        if (!fromExe.empty()) {
            return fromExe;
        }
    }
#endif

    return {};
}

} // namespace daw::internal::core
