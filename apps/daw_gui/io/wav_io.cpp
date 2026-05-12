#include "wav_io.h"

#include "core/CoreState.h"
#include "io/wav.h"

#include <filesystem>

bool IoLoadWavStereo(const std::wstring& path, LoadedAudio* out, std::wstring* error) {
    if (out == nullptr || error == nullptr) return false;
    auto r = daw::io::wav::LoadStereoFromFile(path);
    if (!r) {
        *error = daw::io::wav::Describe(r.error());
        return false;
    }
    auto& buf       = r.value();
    out->sourcePath = path;
    out->displayName = std::filesystem::path(path).stem().wstring();
    out->sampleRate = buf.sampleRate;
    out->frames     = buf.frames;
    out->stereo     = std::move(buf.interleaved);
    return true;
}

bool IoWriteWavPcm16Stereo(const std::wstring& path, const std::vector<float>& stereo, int sampleRate) {
    return static_cast<bool>(daw::io::wav::WritePcm16StereoToFile(path, stereo, sampleRate));
}
