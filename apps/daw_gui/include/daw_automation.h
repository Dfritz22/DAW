#pragma once

struct UiState;

float TrackGainDbAt(const UiState& state, int trackIndex, float beat);
float TrackPanAt(const UiState& state, int trackIndex, float beat);
int TrackBusIndexAt(const UiState& state, int trackIndex, float beat);
float TrackGainDbAt(const UiState& state, int trackIndex);
float TrackPanAt(const UiState& state, int trackIndex);
int TrackBusIndexAt(const UiState& state, int trackIndex);
