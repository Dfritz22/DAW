#pragma once
#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

// ── Phase 22 / Step D — Realtime allocation tracing ──────────────────────────
//
// Opt-in audio-thread allocation guard. When DAW_RT_ALLOC_TRACE is defined at
// build time (e.g. `cmake -DDAW_RT_ALLOC_TRACE=ON`), every realtime audio
// thread (MME output, WASAPI render) wraps its callback loop in a
// Begin/End pair that flips a thread_local flag. A global operator-new
// override in rt_alloc_trace.cpp checks that flag and asserts when an
// allocation happens while the flag is set.
//
// When DAW_RT_ALLOC_TRACE is NOT defined (Release / default Debug builds),
// the Begin/End calls are inline no-ops and the operator-new override is
// not compiled — zero runtime cost.

namespace daw::audio {

#ifdef DAW_RT_ALLOC_TRACE
    void RtBeginAudioThread() noexcept;
    void RtEndAudioThread() noexcept;
    bool RtIsAudioThread() noexcept;
#else
    inline void RtBeginAudioThread() noexcept {}
    inline void RtEndAudioThread() noexcept {}
    inline bool RtIsAudioThread() noexcept { return false; }
#endif

} // namespace daw::audio

// Phase 25 / Step L1 — always-on thread identity. RtAudioThreadScope now
// brackets BOTH the optional DAW_RT_ALLOC_TRACE flag AND the always-on
// daw::threading audio-thread marker, so every existing audio render thread
// is automatically registered for IsAudioThread() / DAW_ASSERT_AUDIO_THREAD
// checks without further plumbing.
#include "threading/thread_identity.h"

namespace daw::audio {

// RAII scope guard.
struct RtAudioThreadScope {
    RtAudioThreadScope() noexcept  { RtBeginAudioThread(); daw::threading::BeginAudioThread(); }
    ~RtAudioThreadScope() noexcept { daw::threading::EndAudioThread(); RtEndAudioThread(); }
    RtAudioThreadScope(const RtAudioThreadScope&) = delete;
    RtAudioThreadScope& operator=(const RtAudioThreadScope&) = delete;
};

} // namespace daw::audio
