#include "core/timeline_edit_ops.h"

#include <algorithm>
#include <cstdint>
#include <string>

namespace daw::core {

bool SplitClip(ProjectData& project, int clipIndex, float splitBeat, float samplesPerBeat) {
    if (clipIndex < 0 || clipIndex >= static_cast<int>(project.clips.size())) return false;
    ClipItem& clip = project.clips[static_cast<size_t>(clipIndex)];
    if (splitBeat <= clip.startBeat + 0.01f ||
        splitBeat >= clip.startBeat + clip.lengthBeats - 0.01f) {
        return false;
    }

    ClipItem right = clip;
    const float splitDelta = splitBeat - clip.startBeat;
    right.startBeat          = splitBeat;
    right.lengthBeats        = clip.lengthBeats - splitDelta;
    right.sourceOffsetFrames = clip.sourceOffsetFrames +
        static_cast<std::uint64_t>(splitDelta * samplesPerBeat);
    clip.lengthBeats         = splitDelta;
    project.clips.push_back(right);
    return true;
}

int DuplicateClip(ProjectData& project, int clipIndex) {
    if (clipIndex < 0 || clipIndex >= static_cast<int>(project.clips.size())) return -1;
    ClipItem dup = project.clips[static_cast<size_t>(clipIndex)];
    dup.startBeat += dup.lengthBeats;
    project.clips.push_back(dup);
    return static_cast<int>(project.clips.size()) - 1;
}

bool NudgeClip(ProjectData& project, int clipIndex, float deltaBeats) {
    if (clipIndex < 0 || clipIndex >= static_cast<int>(project.clips.size())) return false;
    ClipItem& clip = project.clips[static_cast<size_t>(clipIndex)];
    clip.startBeat = std::max(0.0f, clip.startBeat + deltaBeats);
    return true;
}

bool DeleteClip(ProjectData& project, int clipIndex) {
    if (clipIndex < 0 || clipIndex >= static_cast<int>(project.clips.size())) return false;
    project.clips.erase(project.clips.begin() + clipIndex);
    return true;
}

int AppendDefaultTrack(ProjectData& project) {
    const int index = static_cast<int>(project.tracks.size());
    TrackData t{};
    t.name = L"Track " + std::to_wstring(index + 1);
    t.busIndex = 1;
    project.tracks.push_back(std::move(t));
    return index;
}

bool DeleteTrack(ProjectData& project, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(project.tracks.size())) return false;
    project.tracks.erase(project.tracks.begin() + trackIndex);

    for (int i = static_cast<int>(project.clips.size()) - 1; i >= 0; --i) {
        ClipItem& clip = project.clips[static_cast<size_t>(i)];
        if (clip.trackIndex == trackIndex) {
            project.clips.erase(project.clips.begin() + i);
            continue;
        }
        if (clip.trackIndex > trackIndex) {
            clip.trackIndex -= 1;
        }
    }
    return true;
}

} // namespace daw::core
