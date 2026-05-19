#pragma once
#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

// ── Phase 25 / Step L — Thread identity formalization ───────────────────────
//
// Always-on, near-zero-cost predicates that answer "am I currently running on
// the UI/main thread?" or "…on the realtime audio thread?". Used by Step L
// asserts at the entry of WndProc handlers, audio callbacks, orchestration
// helpers, and (eventually) plugin SDK shims.
//
// Implementation:
//   * The main thread registers itself once at WinMain startup via
//     RegisterMainThread(). The thread id is stored in a relaxed atomic and
//     compared by IsMainThread().
//   * Audio render threads (MME / WASAPI) flip a thread_local bool through
//     BeginAudioThread() / EndAudioThread(); the existing RtAudioThreadScope
//     RAII guard in audio/rt_alloc_trace.h forwards into these calls so all
//     existing audio-thread entry points are already wired.
//
// Costs:
//   * IsMainThread(): one atomic load + GetCurrentThreadId().
//   * IsAudioThread(): one thread_local read.
//   * No allocations, no synchronization on the audio thread.

namespace daw::threading {

// Call once from wWinMain BEFORE any other initialization. Subsequent calls
// from the same thread are no-ops; calls from a different thread overwrite
// the recorded id (intended only for unit-test reseating).
void RegisterMainThread() noexcept;

// True when invoked on the thread that called RegisterMainThread().
bool IsMainThread() noexcept;

// True when invoked between BeginAudioThread() and EndAudioThread() on the
// current thread (i.e. inside an AudioThreadScope on a render thread).
bool IsAudioThread() noexcept;

void BeginAudioThread() noexcept;
void EndAudioThread() noexcept;

struct AudioThreadScope {
    AudioThreadScope() noexcept  { BeginAudioThread(); }
    ~AudioThreadScope() noexcept { EndAudioThread(); }
    AudioThreadScope(const AudioThreadScope&) = delete;
    AudioThreadScope& operator=(const AudioThreadScope&) = delete;
};

} // namespace daw::threading
