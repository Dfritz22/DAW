#include "thread_identity.h"

#include <atomic>
#include <cstdint>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace daw::threading {

namespace {
    // 0 = unregistered. GetCurrentThreadId() never returns 0 on Windows, so
    // 0 is safe as a sentinel.
    std::atomic<std::uint32_t> g_mainThreadId{0};

    thread_local int g_audioThreadDepth = 0;
}

void RegisterMainThread() noexcept {
    g_mainThreadId.store(static_cast<std::uint32_t>(::GetCurrentThreadId()),
                         std::memory_order_relaxed);
}

bool IsMainThread() noexcept {
    const auto recorded = g_mainThreadId.load(std::memory_order_relaxed);
    if (recorded == 0) return false;
    return recorded == static_cast<std::uint32_t>(::GetCurrentThreadId());
}

bool IsAudioThread() noexcept {
    return g_audioThreadDepth > 0;
}

void BeginAudioThread() noexcept {
    ++g_audioThreadDepth;
}

void EndAudioThread() noexcept {
    if (g_audioThreadDepth > 0) --g_audioThreadDepth;
}

} // namespace daw::threading
