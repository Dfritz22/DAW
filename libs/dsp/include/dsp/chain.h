#pragma once

#include <vector>
#include "dsp/insert_types.h"

// ── DSP insert chain (frozen contract) ──────────────────────────────────────
// Apply a series of insert effects in-place to an interleaved stereo float
// buffer (`buf` is [L,R,L,R,...]). `effects[i]` selects which processor
// runs in slot `i`; `bypass[i]` skips it; `configs[i]` holds parameter
// values; `states[i]` holds persistent per-slot DSP state (filter taps,
// envelopes, delay lines) that the caller owns and persists across calls.
//
// `slotCount` is clamped to [0, kMaxInsertSlots]. The function performs
// no allocations on the hot path other than what individual processors do
// internally on parameter change (delay-line resize, reverb buffer alloc).
// It does not throw, lock, or touch globals.

void DspApplyInsertChain(
    std::vector<float>& buf, float sampleRate,
    const InsertEffectArray& effects,
    const InsertBypassArray& bypass,
    const InsertConfigArray& configs,
    InsertDspStateArray& states,
    int slotCount);
