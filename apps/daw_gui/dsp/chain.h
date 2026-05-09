#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include <vector>
#include "dsp/insert_types.h"

// DSP insert chain processing.
// All insert types are defined in dsp/insert_types.h.

void DspApplyInsertChain(
    std::vector<float>& buf, float sampleRate,
    const InsertEffectArray& effects,
    const InsertBypassArray& bypass,
    const InsertConfigArray& configs,
    InsertDspStateArray& states,
    int slotCount);
