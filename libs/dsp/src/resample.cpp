#include "dsp/resample.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace daw::dsp {

void ResampleStereoPcm16Linear(const std::int16_t* src, int srcFrames, std::int16_t* dst, int dstFrames) {
    if (src == nullptr || dst == nullptr || srcFrames <= 0 || dstFrames <= 0) {
        return;
    }
    if (srcFrames == 1) {
        const std::int16_t l = src[0];
        const std::int16_t r = src[1];
        for (int i = 0; i < dstFrames; ++i) {
            dst[i * 2] = l;
            dst[i * 2 + 1] = r;
        }
        return;
    }

    const double maxSrcPos = static_cast<double>(srcFrames - 1);
    for (int i = 0; i < dstFrames; ++i) {
        const double t = (dstFrames > 1)
            ? (static_cast<double>(i) * maxSrcPos / static_cast<double>(dstFrames - 1))
            : 0.0;
        const int i0 = std::clamp(static_cast<int>(std::floor(t)), 0, srcFrames - 1);
        const int i1 = std::min(i0 + 1, srcFrames - 1);
        const double frac = t - static_cast<double>(i0);

        const double l = static_cast<double>(src[i0 * 2]) * (1.0 - frac) + static_cast<double>(src[i1 * 2]) * frac;
        const double r = static_cast<double>(src[i0 * 2 + 1]) * (1.0 - frac) + static_cast<double>(src[i1 * 2 + 1]) * frac;
        dst[i * 2] = static_cast<std::int16_t>(std::clamp(std::lrint(l), static_cast<long>(-32768), static_cast<long>(32767)));
        dst[i * 2 + 1] = static_cast<std::int16_t>(std::clamp(std::lrint(r), static_cast<long>(-32768), static_cast<long>(32767)));
    }
}

void ResampleStereoFloatLinear(const float* src, int srcFrames, float* dst, int dstFrames) {
    if (src == nullptr || dst == nullptr || srcFrames <= 0 || dstFrames <= 0) {
        return;
    }
    if (srcFrames == 1) {
        const float l = src[0];
        const float r = src[1];
        for (int i = 0; i < dstFrames; ++i) {
            dst[i * 2]     = l;
            dst[i * 2 + 1] = r;
        }
        return;
    }

    const double maxSrcPos = static_cast<double>(srcFrames - 1);
    for (int i = 0; i < dstFrames; ++i) {
        const double t = (dstFrames > 1)
            ? (static_cast<double>(i) * maxSrcPos / static_cast<double>(dstFrames - 1))
            : 0.0;
        const int i0 = std::clamp(static_cast<int>(std::floor(t)), 0, srcFrames - 1);
        const int i1 = std::min(i0 + 1, srcFrames - 1);
        const float frac = static_cast<float>(t - static_cast<double>(i0));

        const float l0 = src[i0 * 2];
        const float r0 = src[i0 * 2 + 1];
        const float l1 = src[i1 * 2];
        const float r1 = src[i1 * 2 + 1];
        dst[i * 2]     = l0 + (l1 - l0) * frac;
        dst[i * 2 + 1] = r0 + (r1 - r0) * frac;
    }
}

namespace {

double BesselI0(double y) {
    double sum = 1.0;
    double term = 1.0;
    const double y2 = (y * 0.5) * (y * 0.5);
    for (int k = 1; k < 30; ++k) {
        term *= y2 / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

double Sinc(double x) {
    if (std::fabs(x) < 1e-12) return 1.0;
    const double pix = 3.14159265358979323846 * x;
    return std::sin(pix) / pix;
}

} // namespace

void ResampleStereoFloatSincHQ(
    const float* src, int srcFrames, int srcSampleRate,
    float* dst,       int dstFrames, int dstSampleRate)
{
    if (src == nullptr || dst == nullptr ||
        srcFrames <= 0 || dstFrames <= 0 ||
        srcSampleRate <= 0 || dstSampleRate <= 0) {
        return;
    }
    if (srcSampleRate == dstSampleRate && srcFrames == dstFrames) {
        std::copy(src, src + static_cast<std::ptrdiff_t>(srcFrames) * 2, dst);
        return;
    }

    constexpr int    kHalfTaps = 32;          // 64-tap kernel
    constexpr int    kTaps     = kHalfTaps * 2;
    constexpr int    kPhases   = 512;         // sub-sample interpolation table
    constexpr double kBeta     = 8.6;
    const double ratio   = static_cast<double>(dstSampleRate) / static_cast<double>(srcSampleRate);
    const double cutoff  = (ratio < 1.0) ? ratio * 0.95 : 0.95;
    const double halfWidthSrc = static_cast<double>(kHalfTaps) / cutoff;
    const double srcStep      = static_cast<double>(srcSampleRate) / static_cast<double>(dstSampleRate);

    std::vector<float> kernel(static_cast<size_t>(kPhases) * static_cast<size_t>(kTaps), 0.0f);
    const double i0Beta = BesselI0(kBeta);
    for (int p = 0; p < kPhases; ++p) {
        const double phaseFrac = static_cast<double>(p) / static_cast<double>(kPhases);
        double sumW = 0.0;
        for (int k = 0; k < kTaps; ++k) {
            const double dx = phaseFrac - static_cast<double>(k - kHalfTaps + 1);
            double w = 0.0;
            if (dx > -halfWidthSrc && dx < halfWidthSrc) {
                const double xn   = dx / halfWidthSrc;
                const double win  = BesselI0(kBeta * std::sqrt(1.0 - xn * xn)) / i0Beta;
                w = win * Sinc(dx * cutoff) * cutoff;
            }
            kernel[static_cast<size_t>(p) * kTaps + static_cast<size_t>(k)] = static_cast<float>(w);
            sumW += w;
        }
        if (std::fabs(sumW) > 1e-9) {
            const float inv = static_cast<float>(1.0 / sumW);
            for (int k = 0; k < kTaps; ++k) {
                kernel[static_cast<size_t>(p) * kTaps + static_cast<size_t>(k)] *= inv;
            }
        }
    }

    for (int i = 0; i < dstFrames; ++i) {
        const double t        = static_cast<double>(i) * srcStep;
        const int    iCenter  = static_cast<int>(std::floor(t));
        const double frac     = t - static_cast<double>(iCenter);
        const double phasePos = frac * static_cast<double>(kPhases);
        const int    p0       = std::clamp(static_cast<int>(std::floor(phasePos)), 0, kPhases - 1);
        const int    p1       = (p0 + 1 < kPhases) ? (p0 + 1) : p0;
        const float  pf       = static_cast<float>(phasePos - static_cast<double>(p0));

        const float* k0row = &kernel[static_cast<size_t>(p0) * kTaps];
        const float* k1row = &kernel[static_cast<size_t>(p1) * kTaps];

        float sumL = 0.0f, sumR = 0.0f;
        const int srcStart = iCenter - kHalfTaps + 1;
        for (int k = 0; k < kTaps; ++k) {
            const int kk = std::clamp(srcStart + k, 0, srcFrames - 1);
            const float w = k0row[k] + (k1row[k] - k0row[k]) * pf;
            sumL += src[kk * 2]     * w;
            sumR += src[kk * 2 + 1] * w;
        }
        dst[i * 2]     = sumL;
        dst[i * 2 + 1] = sumR;
    }
}

int ResampleStereoPcm16LinearStateful(
    const std::int16_t* src, int srcFrames,
    std::int16_t* dst,       int dstFrames,
    double step,
    double* phase,
    std::int16_t* lastL, std::int16_t* lastR,
    bool* primed)
{
    if (src == nullptr || dst == nullptr || phase == nullptr ||
        lastL == nullptr || lastR == nullptr || primed == nullptr ||
        srcFrames <= 0 || dstFrames <= 0 || step <= 0.0) {
        return 0;
    }

    int srcIdx = 0;
    if (!*primed) {
        *lastL = src[0];
        *lastR = src[1];
        srcIdx = 1;
        *phase = 0.0;
        *primed = true;
    }

    std::int16_t l0 = *lastL;
    std::int16_t r0 = *lastR;
    std::int16_t l1 = (srcIdx < srcFrames) ? src[srcIdx * 2]     : l0;
    std::int16_t r1 = (srcIdx < srcFrames) ? src[srcIdx * 2 + 1] : r0;

    double ph = *phase;
    for (int i = 0; i < dstFrames; ++i) {
        const double l = static_cast<double>(l0) * (1.0 - ph) + static_cast<double>(l1) * ph;
        const double r = static_cast<double>(r0) * (1.0 - ph) + static_cast<double>(r1) * ph;
        dst[i * 2]     = static_cast<std::int16_t>(std::clamp(std::lrint(l), static_cast<long>(-32768), static_cast<long>(32767)));
        dst[i * 2 + 1] = static_cast<std::int16_t>(std::clamp(std::lrint(r), static_cast<long>(-32768), static_cast<long>(32767)));

        ph += step;
        while (ph >= 1.0 && srcIdx < srcFrames) {
            l0 = l1;
            r0 = r1;
            ++srcIdx;
            if (srcIdx < srcFrames) {
                l1 = src[srcIdx * 2];
                r1 = src[srcIdx * 2 + 1];
            }
            ph -= 1.0;
        }
        if (ph >= 1.0) {
            ph = 0.999999;
            l1 = l0;
            r1 = r0;
        }
    }

    *phase = ph;
    *lastL = l0;
    *lastR = r0;

    return srcIdx;
}

} // namespace daw::dsp
