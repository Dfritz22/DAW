#include <gtest/gtest.h>

#include "engine/clip_render.h"

#include <vector>

using daw::core::ClipItem;
using daw::core::LoadedAudio;
using daw::engine::RenderClipsForTrack;

namespace {

// Build a stereo source where left[i] = i+1, right[i] = -(i+1).
LoadedAudio MakeRamp(int sampleRate, std::uint64_t frames) {
    LoadedAudio a;
    a.sampleRate = sampleRate;
    a.frames = static_cast<std::uint32_t>(frames);
    a.stereo.resize(static_cast<std::size_t>(frames) * 2, 0.0f);
    for (std::uint64_t i = 0; i < frames; ++i) {
        a.stereo[static_cast<std::size_t>(i) * 2]     =  static_cast<float>(i + 1);
        a.stereo[static_cast<std::size_t>(i) * 2 + 1] = -static_cast<float>(i + 1);
    }
    return a;
}

ClipItem MakeClip(int trackIndex, int audioIndex, float startBeat, float lengthBeats) {
    ClipItem c;
    c.trackIndex = trackIndex;
    c.audioIndex = audioIndex;
    c.startBeat = startBeat;
    c.lengthBeats = lengthBeats;
    c.sourceOffsetFrames = 0;
    return c;
}

} // namespace

TEST(RenderClipsForTrack, EmptyClipListLeavesBufferUntouched) {
    std::vector<ClipItem> clips;
    std::vector<LoadedAudio> audio;
    std::vector<float> dst(8, 0.5f);
    RenderClipsForTrack(clips, audio, 0, 48000, 24000.0f, 0, dst.data(), 4);
    for (float v : dst) EXPECT_FLOAT_EQ(v, 0.5f);
}

TEST(RenderClipsForTrack, SingleClipFromZeroOffsetMixesAdditively) {
    std::vector<LoadedAudio> audio{ MakeRamp(48000, 8) };
    std::vector<ClipItem> clips{ MakeClip(0, 0, 0.0f, /*lengthBeats*/ 8.0f / 24000.0f) };

    std::vector<float> dst(8 * 2, 1.0f); // pre-existing content
    RenderClipsForTrack(clips, audio, 0, 48000, 24000.0f, 0, dst.data(), 8);

    // Expect: dst[2i]   = 1 + (i+1), dst[2i+1] = 1 + -(i+1)
    for (std::uint64_t i = 0; i < 8; ++i) {
        EXPECT_FLOAT_EQ(dst[i * 2]    , 1.0f + static_cast<float>(i + 1));
        EXPECT_FLOAT_EQ(dst[i * 2 + 1], 1.0f - static_cast<float>(i + 1));
    }
}

TEST(RenderClipsForTrack, FiltersByTrackIndex) {
    std::vector<LoadedAudio> audio{ MakeRamp(48000, 4) };
    std::vector<ClipItem> clips{
        MakeClip(/*track*/ 1, 0, 0.0f, 4.0f / 24000.0f),
        MakeClip(/*track*/ 0, 0, 0.0f, 4.0f / 24000.0f),
    };
    std::vector<float> dst(4 * 2, 0.0f);
    RenderClipsForTrack(clips, audio, /*trackIndex*/ 1, 48000, 24000.0f, 0, dst.data(), 4);
    // Only the track-1 clip is rendered → ramp content present.
    EXPECT_FLOAT_EQ(dst[0], 1.0f);
    EXPECT_FLOAT_EQ(dst[6], 4.0f);
}

TEST(RenderClipsForTrack, SkipsInvalidAudioIndex) {
    std::vector<LoadedAudio> audio{ MakeRamp(48000, 4) };
    std::vector<ClipItem> clips{
        MakeClip(0, -1, 0.0f, 4.0f / 24000.0f),
        MakeClip(0,  9, 0.0f, 4.0f / 24000.0f),
    };
    std::vector<float> dst(4 * 2, 0.0f);
    RenderClipsForTrack(clips, audio, 0, 48000, 24000.0f, 0, dst.data(), 4);
    for (float v : dst) EXPECT_FLOAT_EQ(v, 0.0f);
}

TEST(RenderClipsForTrack, RespectsBufferStartFrameWindow) {
    // Clip at absolute frames [0, 8). Buffer covers [4, 8).
    std::vector<LoadedAudio> audio{ MakeRamp(48000, 8) };
    std::vector<ClipItem> clips{ MakeClip(0, 0, 0.0f, 8.0f / 24000.0f) };

    std::vector<float> dst(4 * 2, 0.0f);
    RenderClipsForTrack(clips, audio, 0, 48000, 24000.0f,
                        /*bufferStartFrame*/ 4, dst.data(), 4);
    // dst[i] should hold source frames 4,5,6,7 → values 5,6,7,8 / -5,-6,-7,-8
    for (std::uint64_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(dst[i * 2]    ,  static_cast<float>(i + 5));
        EXPECT_FLOAT_EQ(dst[i * 2 + 1], -static_cast<float>(i + 5));
    }
}

TEST(RenderClipsForTrack, ClipPartiallyBeforeBufferIsCropped) {
    // Clip at [0, 8), buffer at [2, 6) → middle 4 frames written.
    std::vector<LoadedAudio> audio{ MakeRamp(48000, 8) };
    std::vector<ClipItem> clips{ MakeClip(0, 0, 0.0f, 8.0f / 24000.0f) };

    std::vector<float> dst(4 * 2, 0.0f);
    RenderClipsForTrack(clips, audio, 0, 48000, 24000.0f, 2, dst.data(), 4);
    for (std::uint64_t i = 0; i < 4; ++i) {
        // Source frame (2+i), value (2+i+1)
        EXPECT_FLOAT_EQ(dst[i * 2]    ,  static_cast<float>(i + 3));
        EXPECT_FLOAT_EQ(dst[i * 2 + 1], -static_cast<float>(i + 3));
    }
}

TEST(RenderClipsForTrack, ClipBeyondBufferEndIsCropped) {
    // Clip at [4, 12), buffer [0, 8) → writes to dst frames 4..7 from source 0..3.
    std::vector<LoadedAudio> audio{ MakeRamp(48000, 8) };
    // startBeat = 4 / spb; lengthBeats = 8 / spb; spb = 24000.
    std::vector<ClipItem> clips{ MakeClip(0, 0, 4.0f / 24000.0f, 8.0f / 24000.0f) };

    std::vector<float> dst(8 * 2, 0.0f);
    RenderClipsForTrack(clips, audio, 0, 48000, 24000.0f, 0, dst.data(), 8);
    // dst frames 0..3 untouched
    for (std::uint64_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(dst[i * 2]    , 0.0f);
        EXPECT_FLOAT_EQ(dst[i * 2 + 1], 0.0f);
    }
    // dst frames 4..7 = source 0..3
    for (std::uint64_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(dst[(i + 4) * 2]    ,  static_cast<float>(i + 1));
        EXPECT_FLOAT_EQ(dst[(i + 4) * 2 + 1], -static_cast<float>(i + 1));
    }
}

TEST(RenderClipsForTrack, OverlappingClipsSumAdditively) {
    std::vector<LoadedAudio> audio{ MakeRamp(48000, 4) };
    std::vector<ClipItem> clips{
        MakeClip(0, 0, 0.0f, 4.0f / 24000.0f),
        MakeClip(0, 0, 0.0f, 4.0f / 24000.0f),
    };
    std::vector<float> dst(4 * 2, 0.0f);
    RenderClipsForTrack(clips, audio, 0, 48000, 24000.0f, 0, dst.data(), 4);
    for (std::uint64_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(dst[i * 2]    ,  2.0f * static_cast<float>(i + 1));
        EXPECT_FLOAT_EQ(dst[i * 2 + 1], -2.0f * static_cast<float>(i + 1));
    }
}

TEST(RenderClipsForTrack, NullDstIsNoOp) {
    std::vector<LoadedAudio> audio{ MakeRamp(48000, 4) };
    std::vector<ClipItem> clips{ MakeClip(0, 0, 0.0f, 4.0f / 24000.0f) };
    RenderClipsForTrack(clips, audio, 0, 48000, 24000.0f, 0, nullptr, 4);
    SUCCEED();
}
