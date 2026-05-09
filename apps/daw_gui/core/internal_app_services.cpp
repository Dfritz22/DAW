#include "core/internal_app_services.h"

namespace daw::internal::core {

void UpdateWindowTitle(HWND hwnd, const UiState& state) {
    std::wstring name = state.projectFilePath.empty()
        ? L"Untitled"
        : std::filesystem::path(state.projectFilePath).stem().wstring();
    std::wstring title = L"DAW  -  " + name + (state.projectModified ? L" *" : L"");
    SetWindowTextW(hwnd, title.c_str());
}

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

    wchar_t exeBuf[MAX_PATH] = {};
    const DWORD exeLen = GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    if (exeLen > 0 && exeLen < MAX_PATH) {
        std::filesystem::path exeDir = std::filesystem::path(exeBuf).parent_path();
        std::filesystem::path fromExe = scanUp(exeDir);
        if (!fromExe.empty()) {
            return fromExe;
        }
    }

    wchar_t cwdBuf[MAX_PATH] = {};
    const DWORD cwdLen = GetCurrentDirectoryW(MAX_PATH, cwdBuf);
    if (cwdLen > 0 && cwdLen < MAX_PATH) {
        std::filesystem::path fromCwd = scanUp(std::filesystem::path(cwdBuf));
        if (!fromCwd.empty()) {
            return fromCwd;
        }
    }

    return {};
}

} // namespace daw::internal::core
