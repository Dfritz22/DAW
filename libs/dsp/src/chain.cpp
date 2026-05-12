#include "dsp/chain.h"

#include <algorithm>
#include <cmath>
#include <vector>

// ── DSP insert processing ──────────────────────────────────────────────────
// Stateless offline processing — coefficients recomputed per-call.
// Operates on interleaved stereo float buffer in-place.

static void DspApplyBiquadBand(std::vector<float>& buf, const EqBand& band, EqBandDspState& bandState, float sampleRate) {
    if (buf.size() < 2) return;
    const float f0 = std::clamp(band.freq_hz, 20.0f, sampleRate * 0.499f);
    const float Q  = std::max(0.1f, band.q);
    const float gainLin = std::pow(10.0f, band.gain_db / 40.0f);  // for peak/shelf
    const float w0 = 2.0f * 3.14159265f * f0 / sampleRate;
    const float cosW = std::cos(w0);
    const float sinW = std::sin(w0);
    const float alpha = sinW / (2.0f * Q);

    float b0=1, b1=0, b2=0, a0=1, a1=0, a2=0;

    switch (band.type) {
    case kEqPeak: {
        const float A = gainLin;
        b0 =  1.0f + alpha * A;
        b1 = -2.0f * cosW;
        b2 =  1.0f - alpha * A;
        a0 =  1.0f + alpha / A;
        a1 = -2.0f * cosW;
        a2 =  1.0f - alpha / A;
        break;
    }
    case kEqLowShelf: {
        const float A = gainLin * gainLin;  // linear amplitude
        const float sqA = gainLin;
        b0 =  A * ((A+1) - (A-1)*cosW + 2.0f*sqA*alpha);
        b1 =  2.0f*A * ((A-1) - (A+1)*cosW);
        b2 =  A * ((A+1) - (A-1)*cosW - 2.0f*sqA*alpha);
        a0 =       (A+1) + (A-1)*cosW + 2.0f*sqA*alpha;
        a1 = -2.0f * ((A-1) + (A+1)*cosW);
        a2 =       (A+1) + (A-1)*cosW - 2.0f*sqA*alpha;
        break;
    }
    case kEqHighShelf: {
        const float A = gainLin * gainLin;
        const float sqA = gainLin;
        b0 =  A * ((A+1) + (A-1)*cosW + 2.0f*sqA*alpha);
        b1 = -2.0f*A * ((A-1) + (A+1)*cosW);
        b2 =  A * ((A+1) + (A-1)*cosW - 2.0f*sqA*alpha);
        a0 =       (A+1) - (A-1)*cosW + 2.0f*sqA*alpha;
        a1 =  2.0f * ((A-1) - (A+1)*cosW);
        a2 =       (A+1) - (A-1)*cosW - 2.0f*sqA*alpha;
        break;
    }
    case kEqLowPass: {
        b0 = (1.0f - cosW) * 0.5f;
        b1 =  1.0f - cosW;
        b2 = (1.0f - cosW) * 0.5f;
        a0 =  1.0f + alpha;
        a1 = -2.0f * cosW;
        a2 =  1.0f - alpha;
        break;
    }
    case kEqHighPass: {
        b0 =  (1.0f + cosW) * 0.5f;
        b1 = -(1.0f + cosW);
        b2 =  (1.0f + cosW) * 0.5f;
        a0 =   1.0f + alpha;
        a1 =  -2.0f * cosW;
        a2 =   1.0f - alpha;
        break;
    }
    default: return;
    }

    // Skip if effectively flat (gain near 0 dB for peak/shelf, Q near default for filters)
    if (band.type == kEqPeak || band.type == kEqLowShelf || band.type == kEqHighShelf) {
        if (std::fabs(band.gain_db) < 0.05f) return;
    }

    b0 /= a0; b1 /= a0; b2 /= a0;
    a1 /= a0; a2 /= a0;

    // Process left and right channels with persistent state
    const size_t frames = buf.size() / 2;
    for (int ch = 0; ch < 2; ++ch) {
        float& x1 = bandState.bq_x1[ch]; float& x2 = bandState.bq_x2[ch];
        float& y1 = bandState.bq_y1[ch]; float& y2 = bandState.bq_y2[ch];
        for (size_t f = 0; f < frames; ++f) {
            const float x0 = buf[f*2 + static_cast<size_t>(ch)];
            const float y0 = b0*x0 + b1*x1 + b2*x2 - a1*y1 - a2*y2;
            x2=x1; x1=x0; y2=y1; y1=y0;
            buf[f*2 + static_cast<size_t>(ch)] = y0;
        }
    }
}

static void DspApplyEQ(std::vector<float>& buf, const InsertConfig& config, InsertDspState& state, float sampleRate) {
    for (int b = 0; b < kEqBandCount; ++b) {
        DspApplyBiquadBand(buf, config.eq[b], state.eq[b], sampleRate);
    }
}

static void DspApplyCompressor(std::vector<float>& buf, const InsertConfig& config, InsertDspState& state, float sampleRate) {
    if (buf.size() < 2) return;
    const float ratio = std::max(1.0f, config.cmp_ratio);
    const float makeup = std::pow(10.0f, config.cmp_makeup_db / 20.0f);
    const float attackCoef  = std::exp(-1.0f / (config.cmp_attack_ms   * 0.001f * sampleRate));
    const float releaseCoef = std::exp(-1.0f / (config.cmp_release_ms  * 0.001f * sampleRate));
    const float kneeHalf = config.cmp_knee_db * 0.5f;

    float& env = state.cmp_env;  // persistent across blocks
    const size_t frames = buf.size() / 2;
    for (size_t f = 0; f < frames; ++f) {
        const float L = buf[f*2];
        const float R = buf[f*2+1];
        const float peak = std::max(std::fabs(L), std::fabs(R));
        // Envelope follower
        if (peak > env)
            env = attackCoef  * env + (1.0f - attackCoef)  * peak;
        else
            env = releaseCoef * env + (1.0f - releaseCoef) * peak;

        const float envDb = (env > 1e-6f) ? 20.0f * std::log10(env) : -120.0f;
        const float thDb  = config.cmp_threshold_db;

        float gainDb = 0.0f;
        const float over = envDb - thDb;
        if (over > kneeHalf) {
            gainDb = (1.0f - 1.0f / ratio) * (thDb + kneeHalf - envDb);
        } else if (over > -kneeHalf && kneeHalf > 0.0f) {
            const float t = (over + kneeHalf) / (2.0f * kneeHalf);
            gainDb = (1.0f - 1.0f / ratio) * t * t * (kneeHalf - envDb + thDb);
        }
        const float gr = std::pow(10.0f, gainDb / 20.0f) * makeup;
        buf[f*2]   *= gr;
        buf[f*2+1] *= gr;
    }
}

static void DspApplySaturation(std::vector<float>& buf, const InsertConfig& config) {
    if (buf.size() < 2) return;
    const float drive = std::clamp(config.sat_drive, 0.0f, 1.0f);
    const float mix   = std::clamp(config.sat_mix, 0.0f, 1.0f);
    if (drive < 0.001f || mix < 0.001f) return;
    const float gain = 1.0f + drive * 9.0f;  // up to 10x drive
    for (size_t i = 0; i < buf.size(); ++i) {
        const float dry = buf[i];
        const float driven = dry * gain;
        // Soft clip: 2/pi * atan
        const float wet = (2.0f / 3.14159265f) * std::atan(driven * 1.5708f);
        buf[i] = dry * (1.0f - mix) + wet * mix;
    }
}

static void DspApplyDelay(std::vector<float>& buf, const InsertConfig& config, InsertDspState& state, float sampleRate) {
    if (buf.size() < 2) return;
    const int delayFrames = std::max(1, static_cast<int>(config.dly_time_ms * 0.001f * sampleRate));
    const float fb  = std::clamp(config.dly_feedback, 0.0f, 0.95f);
    const float mix = std::clamp(config.dly_mix, 0.0f, 1.0f);
    if (mix < 0.001f) return;

    // Resize delay lines only when the delay time changes (preserves tail)
    if (state.dly_lastFrames != delayFrames) {
        state.dly_bufL.assign(static_cast<size_t>(delayFrames), 0.0f);
        state.dly_bufR.assign(static_cast<size_t>(delayFrames), 0.0f);
        state.dly_wpos       = 0;
        state.dly_lastFrames = delayFrames;
    }

    const size_t frames = buf.size() / 2;
    const int dframes = delayFrames;
    for (size_t f = 0; f < frames; ++f) {
        const float dryL = buf[f*2];
        const float dryR = buf[f*2+1];
        // Ping-pong: L reads from R delay line, R reads from L delay line
        const float wetL = state.dly_bufR[static_cast<size_t>(state.dly_wpos)];
        const float wetR = state.dly_bufL[static_cast<size_t>(state.dly_wpos)];
        state.dly_bufL[static_cast<size_t>(state.dly_wpos)] = dryL + wetR * fb;
        state.dly_bufR[static_cast<size_t>(state.dly_wpos)] = dryR + wetL * fb;
        state.dly_wpos = (state.dly_wpos + 1) % dframes;
        buf[f*2]   = dryL * (1.0f - mix) + wetL * mix;
        buf[f*2+1] = dryR * (1.0f - mix) + wetR * mix;
    }
}

static void DspApplyReverb(std::vector<float>& buf, const InsertConfig& config, InsertDspState& state, float sampleRate) {
    if (buf.size() < 2) return;
    const float mix = std::clamp(config.rev_mix, 0.0f, 1.0f);
    if (mix < 0.001f) return;

    // Simple Schroeder reverb: 4 comb filters + 2 allpass (persistent state)
    const float room = std::clamp(config.rev_room_size, 0.0f, 1.0f);
    const float damp = std::clamp(config.rev_damping, 0.0f, 1.0f);

    static const float kCombMs[4] = {29.7f, 37.1f, 41.1f, 43.7f};
    static const float kApMs[2]   = {5.0f, 1.7f};

    const int combLen[4] = {
        std::max(1, static_cast<int>(kCombMs[0] * 0.001f * sampleRate * (0.8f + room * 0.4f))),
        std::max(1, static_cast<int>(kCombMs[1] * 0.001f * sampleRate * (0.8f + room * 0.4f))),
        std::max(1, static_cast<int>(kCombMs[2] * 0.001f * sampleRate * (0.8f + room * 0.4f))),
        std::max(1, static_cast<int>(kCombMs[3] * 0.001f * sampleRate * (0.8f + room * 0.4f))),
    };
    const int apLen[2] = {
        std::max(1, static_cast<int>(kApMs[0] * 0.001f * sampleRate)),
        std::max(1, static_cast<int>(kApMs[1] * 0.001f * sampleRate)),
    };

    // Allocate/resize persistent comb and allpass buffers only when room size changes
    for (int c = 0; c < 4; ++c) {
        if (state.rev_lastCombLen[c] != combLen[c]) {
            state.rev_combBuf[c].assign(static_cast<size_t>(combLen[c]), 0.0f);
            state.rev_combPos[c]     = 0;
            state.rev_combFilt[c]    = 0.0f;
            state.rev_lastCombLen[c] = combLen[c];
        }
    }
    for (int a = 0; a < 2; ++a) {
        if (static_cast<int>(state.rev_apBuf[a].size()) != apLen[a]) {
            state.rev_apBuf[a].assign(static_cast<size_t>(apLen[a]), 0.0f);
            state.rev_apPos[a] = 0;
        }
    }

    const float feedback = 0.5f + room * 0.38f;
    const float dampCoef = damp * 0.4f;

    const size_t frames = buf.size() / 2;
    for (size_t f = 0; f < frames; ++f) {
        const float mono = (buf[f*2] + buf[f*2+1]) * 0.5f;
        float revOut = 0.0f;

        for (int c = 0; c < 4; ++c) {
            const size_t cp = static_cast<size_t>(state.rev_combPos[c]);
            const float delayed = state.rev_combBuf[c][cp];
            state.rev_combFilt[c] = delayed * (1.0f - dampCoef) + state.rev_combFilt[c] * dampCoef;
            state.rev_combBuf[c][cp] = mono + state.rev_combFilt[c] * feedback;
            state.rev_combPos[c] = (state.rev_combPos[c] + 1) % combLen[c];
            revOut += delayed;
        }
        revOut *= 0.25f;

        for (int a = 0; a < 2; ++a) {
            const size_t ap = static_cast<size_t>(state.rev_apPos[a]);
            const float delayed = state.rev_apBuf[a][ap];
            state.rev_apBuf[a][ap] = revOut + delayed * 0.5f;
            state.rev_apPos[a] = (state.rev_apPos[a] + 1) % apLen[a];
            revOut = delayed - revOut * 0.5f;
        }

        buf[f*2]   = buf[f*2]   * (1.0f - mix) + revOut * mix;
        buf[f*2+1] = buf[f*2+1] * (1.0f - mix) + revOut * mix;
    }
}

static void DspApplyGate(std::vector<float>& buf, const InsertConfig& config, InsertDspState& state, float sampleRate) {
    if (buf.size() < 2) return;
    const float threshold = std::pow(10.0f, config.gate_threshold_db / 20.0f);
    const float attackCoef  = std::exp(-1.0f / (config.gate_attack_ms   * 0.001f * sampleRate));
    const float releaseCoef = std::exp(-1.0f / (config.gate_release_ms  * 0.001f * sampleRate));
    const float holdSamples = config.gate_hold_ms * 0.001f * sampleRate;

    float& env      = state.gate_env;        // persistent across blocks
    float& holdTimer= state.gate_holdTimer;
    float& gateGain = state.gate_gainState;
    const size_t frames = buf.size() / 2;
    for (size_t f = 0; f < frames; ++f) {
        const float peak = std::max(std::fabs(buf[f*2]), std::fabs(buf[f*2+1]));
        if (peak > env)
            env = attackCoef * env + (1.0f - attackCoef) * peak;
        else
            env = releaseCoef * env + (1.0f - releaseCoef) * peak;

        const bool open = env >= threshold;
        if (open) {
            holdTimer = holdSamples;
            gateGain += (1.0f - gateGain) * (1.0f - attackCoef);
        } else if (holdTimer > 0.0f) {
            holdTimer -= 1.0f;
        } else {
            gateGain += (0.0f - gateGain) * (1.0f - releaseCoef);
        }
        buf[f*2]   *= gateGain;
        buf[f*2+1] *= gateGain;
    }
}

static void DspApplyDeEsser(std::vector<float>& buf, const InsertConfig& config, InsertDspState& state, float sampleRate) {
    if (buf.size() < 2) return;
    // Sidechain: bandpass around dee_freq_hz, then compress just that band
    const float threshold = std::pow(10.0f, config.dee_threshold_db / 20.0f);
    const float reduction = std::clamp(config.dee_reduction_db, 0.0f, 40.0f);
    const float reductionLin = std::pow(10.0f, -reduction / 20.0f);

    // Build sidechain biquad coefficients (peak at sibilance freq)
    const float scFreq = std::clamp(config.dee_freq_hz, 1000.0f, sampleRate * 0.499f);
    const float scQ    = scFreq / std::max(1.0f, config.dee_bandwidth_hz);
    const float w0  = 2.0f * 3.14159265f * scFreq / sampleRate;
    const float cosW = std::cos(w0);
    const float alpha = std::sin(w0) / (2.0f * scQ);
    const float b0sc =  alpha,  b1sc = 0.0f,  b2sc = -alpha;
    const float a0sc =  1.0f + alpha,  a1sc = -2.0f * cosW,  a2sc = 1.0f - alpha;
    const float nb0 = b0sc/a0sc, nb1 = b1sc/a0sc, nb2 = b2sc/a0sc;
    const float na1 = a1sc/a0sc, na2 = a2sc/a0sc;

    // Process sidechain copy using persistent biquad state
    std::vector<float> scBuf = buf;
    for (int ch = 0; ch < 2; ++ch) {
        float& x1 = state.dee_sc_x1[ch]; float& x2 = state.dee_sc_x2[ch];
        float& y1 = state.dee_sc_y1[ch]; float& y2 = state.dee_sc_y2[ch];
        const size_t frames2 = scBuf.size() / 2;
        for (size_t f = 0; f < frames2; ++f) {
            const float x0 = scBuf[f*2 + static_cast<size_t>(ch)];
            const float y0 = nb0*x0 + nb1*x1 + nb2*x2 - na1*y1 - na2*y2;
            x2=x1; x1=x0; y2=y1; y1=y0;
            scBuf[f*2 + static_cast<size_t>(ch)] = y0;
        }
    }

    // Apply gain reduction based on sidechain envelope (fast attack, 20ms release)
    const float attackCoef  = std::exp(-1.0f / (0.5f  * 0.001f * sampleRate));  // 0.5ms
    const float releaseCoef = std::exp(-1.0f / (20.0f * 0.001f * sampleRate));  // 20ms
    float& env = state.dee_env;  // persistent across blocks
    const size_t frames = buf.size() / 2;
    for (size_t f = 0; f < frames; ++f) {
        const float peak = std::max(std::fabs(scBuf[f*2]), std::fabs(scBuf[f*2+1]));
        if (peak > env)
            env = attackCoef  * env + (1.0f - attackCoef)  * peak;
        else
            env = releaseCoef * env + (1.0f - releaseCoef) * peak;

        if (env > threshold) {
            const float gr = 1.0f - (1.0f - reductionLin) * std::min(1.0f, (env - threshold) / threshold);
            buf[f*2]   *= gr;
            buf[f*2+1] *= gr;
        }
    }
}

static void DspApplyLimiter(std::vector<float>& buf, const InsertConfig& config, InsertDspState& state, float sampleRate) {
    if (buf.size() < 2) return;
    const float ceiling = std::pow(10.0f, config.lim_ceiling_db / 20.0f);
    const float releaseCoef = std::exp(-1.0f / (config.lim_release_ms * 0.001f * sampleRate));
    // 0.1ms lookahead approximated as instant attack
    float& env = state.lim_env;  // persistent across blocks
    const size_t frames = buf.size() / 2;
    for (size_t f = 0; f < frames; ++f) {
        const float peak = std::max(std::fabs(buf[f*2]), std::fabs(buf[f*2+1]));
        if (peak > env)
            env = peak;
        else
            env = releaseCoef * env + (1.0f - releaseCoef) * peak;
        const float gr = (env > ceiling) ? (ceiling / env) : 1.0f;
        buf[f*2]   *= gr;
        buf[f*2+1] *= gr;
    }
}

// Apply a full insert chain to an interleaved stereo buffer.
void DspApplyInsertChain(
    std::vector<float>& buf, float sampleRate,
    const InsertEffectArray& effects,
    const InsertBypassArray& bypass,
    const InsertConfigArray& configs,
    InsertDspStateArray& states,
    int slotCount)
{
    const int count = std::clamp(slotCount, 0, kMaxInsertSlots);
    for (int s = 0; s < count; ++s) {
        if (bypass[static_cast<size_t>(s)]) continue;
        const int fxType = std::clamp(static_cast<int>(effects[static_cast<size_t>(s)]), 0, kInsertEffectTypeCount - 1);
        const InsertConfig& config = configs[static_cast<size_t>(s)];
        InsertDspState& state = states[static_cast<size_t>(s)];
        switch (fxType) {
        case kFxEQ:  DspApplyEQ(buf, config, state, sampleRate);           break;
        case kFxCMP: DspApplyCompressor(buf, config, state, sampleRate);   break;
        case kFxSAT: DspApplySaturation(buf, config);                       break;
        case kFxDLY: DspApplyDelay(buf, config, state, sampleRate);         break;
        case kFxREV: DspApplyReverb(buf, config, state, sampleRate);        break;
        case kFxGATE:DspApplyGate(buf, config, state, sampleRate);          break;
        case kFxDEE: DspApplyDeEsser(buf, config, state, sampleRate);       break;
        case kFxLIM: DspApplyLimiter(buf, config, state, sampleRate);       break;
        default: break;
        }
    }
}
