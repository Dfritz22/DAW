#pragma once
#include <vector>
#include "../state.h"

// DSP insert chain processing.
// All types (InsertParams, EqBand, InsertEffectArray, etc.) are defined in state.h.

void ApplyInsertChain(
    std::vector<float>& buf, float sampleRate,
    const InsertEffectArray& effects,
    const InsertBypassArray& bypass,
    const InsertParamsArray& params,
    int slotCount);
