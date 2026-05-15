// ─────────────────────────────────────────────────────────────────────────────
// ui/draw/timeline.cpp
//
// Ruler + arrange-lane rendering. Includes file-static waveform helpers
// (EnsurePeakSummary / DrawClipWaveform / DrawLiveRecordingWaveform) used
// only by ArrangeLanes. Lifted verbatim from ui/draw.cpp in Phase 17e.
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/draw/draw_internal.h"
#include "ui/draw.h"
#include "ui/layout.h"
#include "ui/dpi.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace daw::internal::ui;

void UiDrawRuler(HDC hdc, const RECT& rect, const AppState& state) {
    Fill(hdc, rect, kPalette.rulerBg);

    HPEN barPen = CreatePen(PS_SOLID, 1, kPalette.barLine);
    HPEN beatPen = CreatePen(PS_SOLID, 1, kPalette.beatLine);
    HGDIOBJ oldPen = SelectObject(hdc, beatPen);

    const int width = std::max(1, static_cast<int>(rect.right - rect.left));
    const int firstBeat = static_cast<int>(std::floor(state.ui.view.viewStartBeat));
    const int lastBeat = static_cast<int>(std::ceil(state.ui.view.viewStartBeat + state.ui.view.viewBeatsVisible));

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kPalette.textMuted);

    for (int beat = firstBeat; beat <= lastBeat; ++beat) {
        const float rel = (static_cast<float>(beat) - state.ui.view.viewStartBeat) / state.ui.view.viewBeatsVisible;
        const int x = rect.left + static_cast<int>(rel * static_cast<float>(width));
        const bool isBar = (beat % 4 == 0);
        SelectObject(hdc, isBar ? barPen : beatPen);
        MoveToEx(hdc, x, rect.top, nullptr);
        LineTo(hdc, x, rect.bottom);

        if (isBar && x >= rect.left && x <= rect.right) {
            const std::wstring label = std::to_wstring((beat / 4) + 1);
            // Place number to the right of the bar line so the digit is
            // never clipped by the panel edge (e.g. the "1" at the home
            // position used to be cut in half).
            RECT t{x + Dpi(3), rect.top + Dpi(2), x + Dpi(40), rect.bottom - Dpi(2)};
            DrawTextW(hdc, label.c_str(), -1, &t, DT_LEFT | DT_TOP | DT_SINGLELINE);
        }
    }

    SelectObject(hdc, oldPen);
    DeleteObject(barPen);
    DeleteObject(beatPen);

    // Playhead marker - downward-pointing triangle whose base meets the
    // arrange playhead line below. Apex points up.
    const int phX = rect.left + static_cast<int>(
        ((state.ui.view.playheadBeat - state.ui.view.viewStartBeat) / std::max(1.0f, state.ui.view.viewBeatsVisible))
        * static_cast<float>(width));
    if (phX >= rect.left && phX <= rect.right) {
        HBRUSH phBrush = CreateSolidBrush(kPalette.playhead);
        HPEN   phPen   = CreatePen(PS_SOLID, 1, kPalette.playhead);
        HGDIOBJ ob = SelectObject(hdc, phBrush);
        HGDIOBJ op = SelectObject(hdc, phPen);
        const int saved = SaveDC(hdc);
        IntersectClipRect(hdc, rect.left, rect.top, rect.right, rect.bottom);
        // Equilateral triangle: base = 2 * triHalf, height = base * sqrt(3)/2.
        // Wide base at the BOTTOM of the ruler (touching the playhead line
        // below), apex points UP — Reaper-style.
        const int triHalf = Dpi(5);
        const int triHeight = (2 * triHalf * 866) / 1000; // ~base * 0.866
        const int baseY = rect.bottom - 1;
        const POINT tri[3] = {
            {phX - triHalf, baseY},
            {phX + triHalf, baseY},
            {phX,           baseY - triHeight}
        };
        Polygon(hdc, tri, 3);
        RestoreDC(hdc, saved);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(phBrush);
        DeleteObject(phPen);
    }
}

static void EnsurePeakSummary(const LoadedAudio& audio) {
    const std::uint32_t bucket = LoadedAudio::kPeakBucketFrames;
    const size_t expected = (audio.frames + bucket - 1) / bucket;
    if (audio.peakSummary.size() == expected || audio.frames == 0 || audio.stereo.empty()) {
        if (audio.peakSummary.size() == expected) return;
    }
    audio.peakSummary.assign(expected, 0.0f);
    for (std::uint32_t b = 0; b < expected; ++b) {
        const std::uint64_t f0 = static_cast<std::uint64_t>(b) * bucket;
        const std::uint64_t f1 = std::min<std::uint64_t>(f0 + bucket, audio.frames);
        float peak = 0.0f;
        for (std::uint64_t f = f0; f < f1; ++f) {
            const size_t i = static_cast<size_t>(f) * 2;
            if (i + 1 >= audio.stereo.size()) break;
            const float l = std::fabs(audio.stereo[i]);
            const float r = std::fabs(audio.stereo[i + 1]);
            if (l > peak) peak = l;
            if (r > peak) peak = r;
        }
        audio.peakSummary[b] = peak;
    }
}

static void DrawClipWaveform(HDC hdc, const RECT& clipRect, const LoadedAudio& audio, std::uint64_t sourceStartFrame, std::uint64_t sourceEndFrame) {
    const int width = std::max(1, static_cast<int>(clipRect.right - clipRect.left));
    const int height = std::max(1, static_cast<int>(clipRect.bottom - clipRect.top));
    if (width < 2 || height < 4 || audio.frames == 0 || audio.stereo.empty()) {
        return;
    }

    const std::uint64_t totalFrames = static_cast<std::uint64_t>(audio.frames);
    const std::uint64_t startFrame = std::min(sourceStartFrame, totalFrames);
    const std::uint64_t endFrame = std::min(std::max(sourceEndFrame, startFrame + 1), totalFrames);
    if (endFrame <= startFrame) {
        return;
    }

    EnsurePeakSummary(audio);

    const int halfHeight = std::max(1, (height / 2) - 2);
    const int centerY = clipRect.top + (height / 2);

    HPEN wavePen = CreatePen(PS_SOLID, 1, RGB(238, 242, 248));
    HGDIOBJ oldPen = SelectObject(hdc, wavePen);

    const int saved = SaveDC(hdc);
    IntersectClipRect(hdc, clipRect.left, clipRect.top, clipRect.right, clipRect.bottom);

    const std::uint32_t bucket = LoadedAudio::kPeakBucketFrames;
    const size_t bucketCount = audio.peakSummary.size();
    const std::uint64_t sourceSpan = endFrame - startFrame;

    for (int x = clipRect.left; x < clipRect.right; ++x) {
        const int pixelIndex = x - clipRect.left;
        const std::uint64_t frameStart = startFrame + (static_cast<std::uint64_t>(pixelIndex) * sourceSpan) / static_cast<std::uint64_t>(width);
        std::uint64_t frameEnd = startFrame + (static_cast<std::uint64_t>(pixelIndex + 1) * sourceSpan) / static_cast<std::uint64_t>(width);
        frameEnd = std::max(frameEnd, frameStart + 1);
        frameEnd = std::min(frameEnd, endFrame);

        float peak = 0.0f;
        if (!audio.peakSummary.empty()) {
            // Iterate over the buckets covering [frameStart, frameEnd).
            const size_t b0 = static_cast<size_t>(frameStart / bucket);
            const size_t b1 = std::min<size_t>(bucketCount, static_cast<size_t>((frameEnd + bucket - 1) / bucket));
            for (size_t b = b0; b < b1; ++b) {
                const float v = audio.peakSummary[b];
                if (v > peak) peak = v;
            }
        }

        const int amp = static_cast<int>(std::round(peak * static_cast<float>(halfHeight)));
        MoveToEx(hdc, x, centerY - amp, nullptr);
        LineTo(hdc, x, centerY + amp + 1);
    }

    RestoreDC(hdc, saved);
    SelectObject(hdc, oldPen);
    DeleteObject(wavePen);
}

// Draw a live recording waveform from pre-computed float stereo data.
// `framesPerPixel` must be the constant project frames-per-pixel ratio for
// the current view; passing this in (instead of computing it from
// totalFrames/width) keeps each pixel's source-frame range stable as the
// recording grows, which prevents the waveform from jittering left/right.
static void DrawLiveRecordingWaveform(HDC hdc, const RECT& clipRect, const std::vector<float>& stereoWaveform, std::uint64_t totalFrames, double framesPerPixel) {
    const int width = std::max(1, static_cast<int>(clipRect.right - clipRect.left));
    const int height = std::max(1, static_cast<int>(clipRect.bottom - clipRect.top));
    if (width < 2 || height < 4 || totalFrames == 0 || stereoWaveform.empty() || framesPerPixel <= 0.0) {
        return;
    }

    const int halfHeight = std::max(1, (height / 2) - 2);
    const int centerY = clipRect.top + (height / 2);

    HPEN wavePen = CreatePen(PS_SOLID, 1, RGB(238, 242, 248));
    HGDIOBJ oldPen = SelectObject(hdc, wavePen);

    const int saved = SaveDC(hdc);
    IntersectClipRect(hdc, clipRect.left, clipRect.top, clipRect.right, clipRect.bottom);

    for (int x = clipRect.left; x < clipRect.right; ++x) {
        const int pixelIndex = x - clipRect.left;
        const std::uint64_t frameStart = static_cast<std::uint64_t>(static_cast<double>(pixelIndex) * framesPerPixel);
        if (frameStart >= totalFrames) {
            break;
        }
        std::uint64_t frameEnd = static_cast<std::uint64_t>(static_cast<double>(pixelIndex + 1) * framesPerPixel);
        frameEnd = std::max(frameEnd, frameStart + 1);
        frameEnd = std::min(frameEnd, totalFrames);

        float peak = 0.0f;
        for (std::uint64_t f = frameStart; f < frameEnd; ++f) {
            const size_t i = static_cast<size_t>(f) * 2;
            if (i + 1 < stereoWaveform.size()) {
                const float l = std::fabs(stereoWaveform[i]);
                const float r = std::fabs(stereoWaveform[i + 1]);
                peak = std::max(peak, std::max(l, r));
            }
        }

        const int amp = static_cast<int>(std::round(peak * static_cast<float>(halfHeight)));
        MoveToEx(hdc, x, centerY - amp, nullptr);
        LineTo(hdc, x, centerY + amp + 1);
    }

    RestoreDC(hdc, saved);
    SelectObject(hdc, oldPen);
    DeleteObject(wavePen);
}

void UiDrawArrangeLanes(HDC hdc, const RECT& rect, const AppState& state) {
    Fill(hdc, rect, kPalette.arrangeBg);

    const int width = std::max(1, static_cast<int>(rect.right - rect.left));
    const int firstBeat = static_cast<int>(std::floor(state.ui.view.viewStartBeat));
    const int lastBeat = static_cast<int>(std::ceil(state.ui.view.viewStartBeat + state.ui.view.viewBeatsVisible));

    HPEN barPen = CreatePen(PS_SOLID, 1, kPalette.barLine);
    HPEN beatPen = CreatePen(PS_SOLID, 1, kPalette.beatLine);
    HPEN lanePen = CreatePen(PS_SOLID, 1, RGB(40, 44, 50));
    HGDIOBJ oldPen = SelectObject(hdc, lanePen);

    const int laneScrollY = state.ui.view.tracksScrollY;
    const int laneRowH = Dpi(kTrackRowHeight);
    for (size_t i = 0; i < state.core.project.tracks.size(); ++i) {
        const int y = rect.top + static_cast<int>(i) * laneRowH - laneScrollY;
        if (y >= rect.bottom) break;
        if (y + laneRowH <= rect.top) continue;
        RECT lane{rect.left, y, rect.right, y + laneRowH};
        Fill(hdc, lane, (i % 2 == 0) ? kPalette.laneDark : kPalette.laneLight);
        MoveToEx(hdc, lane.left, lane.bottom - 1, nullptr);
        LineTo(hdc, lane.right, lane.bottom - 1);
    }

    for (int beat = firstBeat; beat <= lastBeat; ++beat) {
        const float rel = (static_cast<float>(beat) - state.ui.view.viewStartBeat) / state.ui.view.viewBeatsVisible;
        const int x = rect.left + static_cast<int>(rel * static_cast<float>(width));
        SelectObject(hdc, (beat % 4 == 0) ? barPen : beatPen);
        MoveToEx(hdc, x, rect.top, nullptr);
        LineTo(hdc, x, rect.bottom);
    }

    // Draw live recording clip if currently recording
    if (state.audio.recording && state.audio.recordTrackIndex >= 0 &&
        state.audio.recordTrackIndex < static_cast<int>(state.core.project.tracks.size()) &&
        !state.audio.liveRecordingWaveform.empty()) {

        RECT recordingClipRect{};
        if (UiLayoutClipRectForDraw(rect, state, state.audio.liveRecordingClip, &recordingClipRect)) {
            // Draw clip background
            Fill(hdc, recordingClipRect, state.audio.liveRecordingClip.color);

            // Draw name badge
            const int clipInnerLeft = recordingClipRect.left + 2;
            const int clipInnerRight = recordingClipRect.right - 2;
            const int clipInnerTop = recordingClipRect.top + 2;
            const int clipInnerBottom = recordingClipRect.bottom - 2;
            const int badgeHeight = 15;
            const int badgeMaxWidth = 140;
            RECT labelRect{clipInnerLeft + 2, clipInnerTop + 1,
                          std::min(clipInnerRight - 2, clipInnerLeft + badgeMaxWidth),
                          clipInnerTop + 1 + badgeHeight};
            Fill(hdc, labelRect, RGB(20, 23, 28));
            StrokeRect(hdc, labelRect, RGB(60, 66, 74));

            // Draw waveform
            RECT waveRect{clipInnerLeft, labelRect.bottom + 1, clipInnerRight, clipInnerBottom};
            if (waveRect.right > waveRect.left && waveRect.bottom > waveRect.top) {
                const std::uint64_t totalFrames = state.audio.liveRecordingFramesProcessed;
                if (totalFrames > 0) {
                    // Use a constant frames-per-pixel derived from the view
                    // (samples-per-beat and pixels-per-beat are both stable
                    // during recording). This keeps each pixel's source range
                    // fixed as the clip grows, so the waveform doesn't jitter.
                    const double samplesPerBeat = static_cast<double>(SamplesPerBeat(state));
                    const double pixelsPerBeat = static_cast<double>(width) /
                        static_cast<double>(std::max(0.0001f, state.ui.view.viewBeatsVisible));
                    const double framesPerPixel = (pixelsPerBeat > 0.0)
                        ? (samplesPerBeat / pixelsPerBeat)
                        : 0.0;
                    DrawLiveRecordingWaveform(hdc, waveRect, state.audio.liveRecordingWaveform, totalFrames, framesPerPixel);
                }
            }

            // Draw border
            StrokeRect(hdc, recordingClipRect, RGB(24, 24, 24));

            // Draw label
            RECT textRect{labelRect.left + 4, labelRect.top, labelRect.right - 4, labelRect.bottom};
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(240, 240, 240));
            DrawTextW(hdc, state.audio.liveRecordingClip.name.c_str(), -1, &textRect,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    for (size_t i = 0; i < state.core.project.clips.size(); ++i) {
        RECT clipRect{};
        if (!UiLayoutClipRectForDraw(rect, state, state.core.project.clips[i], &clipRect)) {
            continue;
        }

        const int clipUnclippedLeft = UiLayoutBeatToX(rect, state, state.core.project.clips[i].startBeat);
        const int clipUnclippedRight = UiLayoutBeatToX(rect, state, state.core.project.clips[i].startBeat + state.core.project.clips[i].lengthBeats);
        const int fullClipPx = std::max(1, clipUnclippedRight - clipUnclippedLeft);
        const int clippedLeft = static_cast<int>(clipRect.left);
        const int clippedRight = static_cast<int>(clipRect.right);
        const int visibleLeftPx = std::max(0, clippedLeft - clipUnclippedLeft);
        const int visibleRightPx = std::min(fullClipPx, visibleLeftPx + std::max(1, clippedRight - clippedLeft));

        Fill(hdc, clipRect, state.core.project.clips[i].color);

        // Name badge at the top, waveform body underneath across full clip width.
        const int clipInnerLeft = clipRect.left + 2;
        const int clipInnerRight = clipRect.right - 2;
        const int clipInnerTop = clipRect.top + 2;
        const int clipInnerBottom = clipRect.bottom - 2;

        const int badgeHeight = 15;
        const int badgeMaxWidth = 140;
        RECT labelRect{clipInnerLeft + 2, clipInnerTop + 1, std::min(clipInnerRight - 2, clipInnerLeft + badgeMaxWidth), clipInnerTop + 1 + badgeHeight};
        Fill(hdc, labelRect, RGB(20, 23, 28));
        StrokeRect(hdc, labelRect, RGB(60, 66, 74));

        RECT waveRect{clipInnerLeft, labelRect.bottom + 1, clipInnerRight, clipInnerBottom};
        if (state.core.project.clips[i].audioIndex >= 0 && state.core.project.clips[i].audioIndex < static_cast<int>(state.core.project.audio.size()) && waveRect.right > waveRect.left && waveRect.bottom > waveRect.top) {
            const LoadedAudio& audio = state.core.project.audio[static_cast<size_t>(state.core.project.clips[i].audioIndex)];
            const std::uint64_t totalFrames = static_cast<std::uint64_t>(audio.frames);
            const float spb = SamplesPerBeat(state);
            const std::uint64_t srcOffset = state.core.project.clips[i].sourceOffsetFrames;
            const std::uint64_t clipLenFrames = std::min(
                static_cast<std::uint64_t>(state.core.project.clips[i].lengthBeats * spb),
                totalFrames > srcOffset ? totalFrames - srcOffset : std::uint64_t{0});
            if (clipLenFrames > 0) {
                const auto fcp = static_cast<std::uint64_t>(std::max(1, fullClipPx));
                std::uint64_t srcStart = srcOffset + (static_cast<std::uint64_t>(visibleLeftPx)  * clipLenFrames) / fcp;
                std::uint64_t srcEnd   = srcOffset + (static_cast<std::uint64_t>(visibleRightPx) * clipLenFrames) / fcp;
                srcEnd = std::min(std::max(srcEnd, srcStart + 1), totalFrames);
                DrawClipWaveform(hdc, waveRect, audio, srcStart, srcEnd);
            }
        }
        StrokeRect(hdc, clipRect, (state.ui.view.selectedClipIndex == static_cast<int>(i)) ? RGB(232, 232, 232) : RGB(24, 24, 24));

        // Trim edge handles on selected clip
        if (state.ui.view.selectedClipIndex == static_cast<int>(i)) {
            const int handleW = 5;
            RECT leftHandle  {clipRect.left,            clipRect.top, clipRect.left + handleW,  clipRect.bottom};
            RECT rightHandle {clipRect.right - handleW, clipRect.top, clipRect.right,            clipRect.bottom};
            Fill(hdc, leftHandle,  RGB(255, 255, 255));
            Fill(hdc, rightHandle, RGB(255, 255, 255));
        }

        RECT textRect{labelRect.left + 4, labelRect.top, labelRect.right - 4, labelRect.bottom};
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(240, 240, 240));
        DrawTextW(hdc, state.core.project.clips[i].name.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    int playheadX = UiLayoutBeatToX(rect, state, state.ui.view.playheadBeat);
    HPEN playheadPen = CreatePen(PS_SOLID, 2, kPalette.playhead);
    SelectObject(hdc, playheadPen);
    if (playheadX >= rect.left && playheadX <= rect.right) {
        RECT lr{
            std::max<LONG>(playheadX - 1, rect.left),
            rect.top,
            std::min<LONG>(playheadX + 1, rect.right),
            rect.bottom
        };
        if (lr.right > lr.left) {
            Fill(hdc, lr, kPalette.playhead);
        }
    }

    DeleteObject(playheadPen);
    DeleteObject(barPen);
    DeleteObject(beatPen);
    DeleteObject(lanePen);
    SelectObject(hdc, oldPen);

    if (state.core.project.tracks.empty()) {
        RECT hint{rect.left + 24, rect.top + 24, rect.right - 24, rect.top + 80};
        SetTextColor(hdc, kPalette.textMuted);
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, L"Import one or more WAV files to create real tracks and clips.", -1, &hint, DT_LEFT | DT_WORDBREAK);
    }
}
