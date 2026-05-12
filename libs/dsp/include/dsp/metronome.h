#pragma once

#include <cstdint>

namespace daw::dsp {

// Stereo 4/4 metronome click generator. Mixes click impulses additively into
// `outStereo` (interleaved L,R; size = frames*2). Each click is an
// exponentially-decaying sinusoid: 1800 Hz on accented (every 4th) beats,
// 1200 Hz otherwise; click length is 40 ms; output is summed (callers should
// have already cleared or filled outStereo).
//
//   outStereo        Destination buffer (size >= frames*2). Mixed into.
//   frames           Number of stereo frames to render.
//   sampleRate       Output sample rate, Hz.
//   samplesPerBeat   Length of one beat at the project tempo, in samples.
//   baseFrame        Global frame index of the first sample in outStereo.
//                    Used to locate beat boundaries (beatIdx = floor(baseFrame
//                    / samplesPerBeat)). Pass the playback cursor for play/
//                    record, or the count-in cursor for count-in clicks.
//   gain             Linear gain multiplier applied on top of the per-click
//                    base amplitudes (default scale used by the engine = 3.0).
//
// Pure: takes no app state. Realtime-safe (no allocation).
void RenderMetronomeClicks(
    float* outStereo,
    int frames,
    float sampleRate,
    float samplesPerBeat,
    std::uint64_t baseFrame,
    float gain);

} // namespace daw::dsp
