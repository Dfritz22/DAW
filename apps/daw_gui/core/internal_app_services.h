#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include "core/CoreState.h"
#include "dsp/insert_types.h"

#include <filesystem>
#include <string>

struct HWND__;
using HWND = HWND__*;

namespace daw::internal::core {

void UpdateWindowTitle(HWND hwnd, const CoreState& core);

InsertEffectArray DefaultInsertEffects();
InsertBypassArray DefaultInsertBypass();
InsertConfigArray DefaultInsertConfig();

std::wstring QuoteArg(const std::wstring& s);
std::filesystem::path FindRepoRoot();

} // namespace daw::internal::core
