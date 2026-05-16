#include "rt_alloc_trace.h"

// This file compiles to an empty TU unless DAW_RT_ALLOC_TRACE is defined.
// When defined, it provides:
//   1. A thread_local flag flipped by RtBeginAudioThread/RtEndAudioThread.
//   2. Global operator new/new[]/delete/delete[] overrides that assert when
//      called while the flag is set on the current thread.
//
// The overrides forward to std::malloc/std::free; they only add an assert
// gate, so non-audio threads pay only a single TLS load + branch per alloc.

#ifdef DAW_RT_ALLOC_TRACE

#include <cassert>
#include <cstdlib>
#include <new>

namespace {
    thread_local bool g_inAudioThread = false;

    void* RtTrackedAlloc(std::size_t n) {
        // Hard contract: NO allocations on the audio thread. Triggering this
        // assert means a callee on the realtime path heap-allocated; hoist
        // the buffer to AudioRuntimeState or use a fixed-size container.
        assert(!g_inAudioThread
               && "DAW_RT_ALLOC_TRACE: heap allocation on audio thread");
        void* p = std::malloc(n == 0 ? 1 : n);
        if (p == nullptr) throw std::bad_alloc{};
        return p;
    }
}

namespace daw::audio {
    void RtBeginAudioThread() noexcept { g_inAudioThread = true; }
    void RtEndAudioThread() noexcept   { g_inAudioThread = false; }
    bool RtIsAudioThread() noexcept    { return g_inAudioThread; }
} // namespace daw::audio

// Global replacements. Throwing forms are mandatory; nothrow + sized
// variants forward to the throwing form via catch.

void* operator new(std::size_t n)            { return RtTrackedAlloc(n); }
void* operator new[](std::size_t n)          { return RtTrackedAlloc(n); }
void  operator delete(void* p) noexcept      { std::free(p); }
void  operator delete[](void* p) noexcept    { std::free(p); }
void  operator delete(void* p, std::size_t) noexcept   { std::free(p); }
void  operator delete[](void* p, std::size_t) noexcept { std::free(p); }

#endif // DAW_RT_ALLOC_TRACE
