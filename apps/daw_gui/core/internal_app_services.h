#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include "core/state.h"

namespace daw::internal::core {

void UpdateWindowTitle(HWND hwnd, const UiState& state);

InsertEffectArray DefaultInsertEffects();
InsertBypassArray DefaultInsertBypass();
InsertConfigArray DefaultInsertConfig();

std::wstring QuoteArg(const std::wstring& s);
std::filesystem::path FindRepoRoot();

} // namespace daw::internal::core
