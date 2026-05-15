#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include "core/CoreState.h"
#include "audio/AudioRuntimeState.h"
#include "ui/UiRuntimeState.h"

struct AppState {
    CoreState core;
    AudioRuntimeState audio;
    UiRuntimeState ui;
    HWND hwnd {nullptr};   // Phase 18f: hoisted from UiViewState. Main window handle.
};

// ── Forward declarations ────────────────────────────────────────────────────
float SamplesPerBeat(const CoreState& state);
std::uint64_t FramesFromBeats(const CoreState& state, float beat);
float BeatsFromFrames(const CoreState& state, std::uint64_t frame);
float TimelineSamplesPerBeat(const CoreState& state);
std::uint64_t TimelineFramesFromBeats(const CoreState& state, float beat);
float TimelineBeatsFromFrames(const CoreState& state, std::uint64_t frame);
float TrackGainDbAt(const CoreState& state, int trackIndex, float beat);
float TrackPanAt(const CoreState& state, int trackIndex, float beat);
int TrackBusIndexAt(const CoreState& state, int trackIndex, float beat);
float TrackGainDbAt(const CoreState& state, int trackIndex);
float TrackPanAt(const CoreState& state, int trackIndex);
int TrackBusIndexAt(const CoreState& state, int trackIndex);
std::uint64_t ComputeProjectEndFrameLocked(const CoreState& core);

bool RenderTrackToStereoLocked(const CoreState& core, AudioRuntimeState& audio, int trackIndex, std::vector<float>* outStereo, int* outSampleRate);
bool RenderFullMixToStereoLocked(const CoreState& core, AudioRuntimeState& audio, std::vector<float>* outStereo, int* outSampleRate);
bool RenderBusStemToStereoLocked(const CoreState& core, AudioRuntimeState& audio, int busIndex, std::vector<float>* outStereo, int* outSampleRate);
inline bool RenderTrackToStereoLocked(const AppState& state, int trackIndex, std::vector<float>* outStereo, int* outSampleRate) {
    return RenderTrackToStereoLocked(state.core, const_cast<AudioRuntimeState&>(state.audio), trackIndex, outStereo, outSampleRate);
}
inline bool RenderFullMixToStereoLocked(const AppState& state, std::vector<float>* outStereo, int* outSampleRate) {
    return RenderFullMixToStereoLocked(state.core, const_cast<AudioRuntimeState&>(state.audio), outStereo, outSampleRate);
}
inline bool RenderBusStemToStereoLocked(const AppState& state, int busIndex, std::vector<float>* outStereo, int* outSampleRate) {
    return RenderBusStemToStereoLocked(state.core, const_cast<AudioRuntimeState&>(state.audio), busIndex, outStereo, outSampleRate);
}
bool DoExportMix(HWND hwnd, AppState& state);
bool DoMixReadiness(HWND hwnd, AppState& state);
bool DoAutoMaster(HWND hwnd, AppState& state);
const wchar_t* BusName(int busIndex);
float BusGainDbAt(const CoreState& core, int busIndex);
float BusPanAt(const CoreState& core, int busIndex);
bool BusMuteAt(const CoreState& core, int busIndex);
inline float BusGainDbAt(const AppState& state, int busIndex) { return BusGainDbAt(state.core, busIndex); }
inline float BusPanAt(const AppState& state, int busIndex) { return BusPanAt(state.core, busIndex); }
inline bool BusMuteAt(const AppState& state, int busIndex) { return BusMuteAt(state.core, busIndex); }

float SamplesPerBeat(const AppState& app);
std::uint64_t FramesFromBeats(const AppState& app, float beat);
float BeatsFromFrames(const AppState& app, std::uint64_t frame);
inline float TimelineSamplesPerBeat(const AppState& state) { return TimelineSamplesPerBeat(state.core); }
inline std::uint64_t TimelineFramesFromBeats(const AppState& state, float beat) { return TimelineFramesFromBeats(state.core, beat); }
inline float TimelineBeatsFromFrames(const AppState& state, std::uint64_t frame) { return TimelineBeatsFromFrames(state.core, frame); }
float TrackGainDbAt(const AppState& app, int trackIndex, float beat);
float TrackPanAt(const AppState& app, int trackIndex, float beat);
int TrackBusIndexAt(const AppState& app, int trackIndex, float beat);
float TrackGainDbAt(const AppState& app, int trackIndex);
float TrackPanAt(const AppState& app, int trackIndex);
int TrackBusIndexAt(const AppState& app, int trackIndex);
inline std::uint64_t ComputeProjectEndFrameLocked(const AppState& state) { return ComputeProjectEndFrameLocked(state.core); }
