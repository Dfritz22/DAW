#pragma once
#include <vector>
#include "core/state.h"

// DSP insert chain processing.
// All types (InsertParams, EqBand, InsertEffectArray, etc.) are defined in state.h.

void DspApplyInsertChain(
    std::vector<float>& buf, float sampleRate,
    const InsertEffectArray& effects,
    const InsertBypassArray& bypass,
    const InsertConfigArray& configs,
    InsertDspStateArray& states,
    int slotCount);
