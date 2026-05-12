#pragma once

#include <cstdint>

namespace daw::dsp {

// Equal-power (constant-power) stereo pan law.
//   pan = -1.0 → full left  (gainL=1, gainR=0)
//   pan =  0.0 → centre     (gainL=gainR≈0.7071)
//   pan = +1.0 → full right (gainL=0, gainR=1)
// `pan` is clamped internally to [-1, 1]. Output pointers must be non-null.
void EqualPowerPan(float pan, float* outGainL, float* outGainR);

// Combine a linear gain with an equal-power pan into final stereo gains:
//   *outGainL = gainLin * EqualPowerPan(pan).L
//   *outGainR = gainLin * EqualPowerPan(pan).R
// `pan` is clamped to [-1, 1]. Output pointers must be non-null.
// Pure; realtime-safe.
void ApplyGainAndPan(float gainLin, float pan, float* outGainL, float* outGainR);

// Convert a float buffer to interleaved int16 PCM with per-sample clamping
// to [-1, 1]. `sampleCount` is the total number of float samples (= frames*2
// for stereo). Pure; realtime-safe.
void FloatToPcm16Clamped(const float* src, std::int16_t* dst, int sampleCount);

// Additively mix an int16 PCM input buffer (mono or interleaved stereo) into
// a float interleaved-stereo destination, with linear gain. Mono sources are
// duplicated to both channels.
//   src               Source samples (interleaved if srcChannels==2).
//   srcFrames         Number of input frames available in `src`.
//   srcChannels       1 (mono) or 2 (stereo). Other values are treated as 1.
//   dst               Destination interleaved-stereo float buffer (mixed into).
//   dstFrames         Number of stereo output frames requested.
//   gain              Linear gain (caller is responsible for clamping).
// Returns the number of input frames actually consumed (= min(srcFrames,
// dstFrames)). Pure; realtime-safe.
int MixPcm16InputToFloatStereo(
    const std::int16_t* src, int srcFrames, int srcChannels,
    float* dst, int dstFrames,
    float gain);

// Additively mix an interleaved-stereo float source into an interleaved-stereo
// destination with separate left/right linear gains:
//   dst[2i  ] += src[2i  ] * gainL
//   dst[2i+1] += src[2i+1] * gainR
// Pure; realtime-safe.
void MixAddStereoWithGain(
    const float* src, float* dst, int frames,
    float gainL, float gainR);

// In-place per-sample clamp to [-1, 1]. Used on offline render outputs before
// they are returned to UI/export code. Pure; realtime-safe.
void ClampStereoBuffer(float* buf, int sampleCount);

// Additively mix the gained difference (post − pre) into `dst`:
//   dst[i] += (post[i] − pre[i]) * gain
// `sampleCount` is the total number of float samples (= frames*2 for stereo).
// All buffers must hold at least `sampleCount` samples; null pointers and
// non-positive `sampleCount` are no-ops. Pure; realtime-safe.
void MixDifferentialAddWithGain(
    const float* pre, const float* post, float* dst,
    int sampleCount, float gain);

} // namespace daw::dsp
