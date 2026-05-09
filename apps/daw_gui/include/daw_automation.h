#pragma once

struct AppState;

float TrackGainDbAt(const AppState& app, int trackIndex, float beat);
float TrackPanAt(const AppState& app, int trackIndex, float beat);
int TrackBusIndexAt(const AppState& app, int trackIndex, float beat);
float TrackGainDbAt(const AppState& app, int trackIndex);
float TrackPanAt(const AppState& app, int trackIndex);
int TrackBusIndexAt(const AppState& app, int trackIndex);
