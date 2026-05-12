#include "dsp/mix.h"

#include <algorithm>
#include <cmath>

namespace daw::dsp {

namespace {
constexpr float kPi = 3.14159265358979323846f;
} // namespace

void EqualPowerPan(float pan, float* outGainL, float* outGainR) {
    if (outGainL == nullptr || outGainR == nullptr) {
        return;
    }
    const float clamped = std::clamp(pan, -1.0f, 1.0f);
    const float panRad  = (clamped + 1.0f) * 0.5f * kPi * 0.5f;
    *outGainL = std::cos(panRad);
    *outGainR = std::sin(panRad);
}

void ApplyGainAndPan(float gainLin, float pan, float* outGainL, float* outGainR) {
    if (outGainL == nullptr || outGainR == nullptr) {
        return;
    }
    float panL = 0.0f;
    float panR = 0.0f;
    EqualPowerPan(pan, &panL, &panR);
    *outGainL = gainLin * panL;
    *outGainR = gainLin * panR;
}

void FloatToPcm16Clamped(const float* src, std::int16_t* dst, int sampleCount) {
    if (src == nullptr || dst == nullptr || sampleCount <= 0) {
        return;
    }
    for (int i = 0; i < sampleCount; ++i) {
        const float v = std::clamp(src[i], -1.0f, 1.0f);
        dst[i] = static_cast<std::int16_t>(std::lrint(v * 32767.0f));
    }
}

int MixPcm16InputToFloatStereo(
    const std::int16_t* src, int srcFrames, int srcChannels,
    float* dst, int dstFrames,
    float gain)
{
    if (src == nullptr || dst == nullptr || srcFrames <= 0 || dstFrames <= 0) {
        return 0;
    }
    const int inCh = (srcChannels == 2) ? 2 : 1;
    const int frames = std::min(srcFrames, dstFrames);
    for (int i = 0; i < frames; ++i) {
        float l = 0.0f;
        float r = 0.0f;
        const int base = i * inCh;
        if (inCh == 1) {
            const float v = static_cast<float>(src[base]) / 32768.0f;
            l = v;
            r = v;
        } else {
            l = static_cast<float>(src[base])     / 32768.0f;
            r = static_cast<float>(src[base + 1]) / 32768.0f;
        }
        dst[i * 2]     += l * gain;
        dst[i * 2 + 1] += r * gain;
    }
    return frames;
}

void MixAddStereoWithGain(
    const float* src, float* dst, int frames,
    float gainL, float gainR)
{
    if (src == nullptr || dst == nullptr || frames <= 0) {
        return;
    }
    for (int i = 0; i < frames; ++i) {
        dst[i * 2]     += src[i * 2]     * gainL;
        dst[i * 2 + 1] += src[i * 2 + 1] * gainR;
    }
}

void ClampStereoBuffer(float* buf, int sampleCount) {
    if (buf == nullptr || sampleCount <= 0) {
        return;
    }
    for (int i = 0; i < sampleCount; ++i) {
        buf[i] = std::clamp(buf[i], -1.0f, 1.0f);
    }
}

void MixDifferentialAddWithGain(
    const float* pre, const float* post, float* dst,
    int sampleCount, float gain)
{
    if (pre == nullptr || post == nullptr || dst == nullptr || sampleCount <= 0) {
        return;
    }
    for (int i = 0; i < sampleCount; ++i) {
        dst[i] += (post[i] - pre[i]) * gain;
    }
}

} // namespace daw::dsp
