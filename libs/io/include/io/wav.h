#pragma once

// ── daw::io::wav ────────────────────────────────────────────────────────────
// Stereo WAV codec. Inputs/outputs are pure POD: this module knows nothing
// about AppState, LoadedAudio, the dock tree, or the audio engine. The
// caller is responsible for any app-level metadata (display names, peak
// summaries, source paths, etc.).
//
// Decode formats:  PCM 16/24/32-bit, IEEE float 32-bit, mono or stereo.
// Encode format:   PCM 16-bit stereo only.
//
// The decoded buffer is always interleaved stereo (mono inputs are
// duplicated to both channels), normalized to [-1, 1] floats.
//
// All three pure functions (`ParseStereo`, `EncodePcm16Stereo`) are
// filesystem-free and exercise the full codec; the file wrappers are thin
// I/O adapters around them so unit tests can run entirely in memory.
//
// ── Architectural contract (frozen for the overhaul) ───────────────────────
// This header is the WAV codec's stable API. It must not grow dependencies
// on AppState, Win32 UI, or any layer above `platform`.

#include <cstdint>
#include <string>
#include <vector>

#include "base/result.h"

namespace daw::io::wav {

// Decoded audio buffer, normalized to interleaved-stereo float [-1, 1].
struct StereoBuffer {
    int               sampleRate = 0;
    std::uint32_t     frames     = 0;        // == interleaved.size() / 2
    std::vector<float> interleaved;          // [L, R, L, R, ...]
};

// Codec failure modes. Caller maps these to UI text via `Describe`.
enum class Error {
    OpenFailed,            // file could not be opened for read/write
    ReadFailed,            // I/O error during decode
    WriteFailed,           // I/O error during encode
    TooSmall,              // file shorter than the smallest valid WAV
    NotRiffWave,           // RIFF/WAVE magic missing
    MissingChunks,         // fmt or data chunk missing
    UnsupportedChannels,   // channels other than 1 or 2
    UnsupportedFormat,     // codec is not PCM 16/24/32 or float 32
    InvalidArguments,      // null/empty input, odd sample count, sr <= 0
};

// Human-readable English description of an error code.
const wchar_t* Describe(Error e);

// ── Pure (no I/O) ───────────────────────────────────────────────────────────

// Decode an in-memory WAV byte buffer into a StereoBuffer.
base::Result<StereoBuffer, Error>
ParseStereo(const std::vector<std::uint8_t>& bytes);

// Encode an interleaved-stereo float buffer as a complete WAV file image
// (header + PCM 16-bit samples). `interleaved.size()` must be even and
// `sampleRate` > 0.
base::Result<std::vector<std::uint8_t>, Error>
EncodePcm16Stereo(const std::vector<float>& interleaved, int sampleRate);

// ── File wrappers ──────────────────────────────────────────────────────────

// Read `path`, then `ParseStereo` the bytes.
base::Result<StereoBuffer, Error>
LoadStereoFromFile(const std::wstring& path);

// `EncodePcm16Stereo`, then write the bytes to `path` (truncating).
base::Result<void, Error>
WritePcm16StereoToFile(const std::wstring& path,
                       const std::vector<float>& interleaved,
                       int sampleRate);

} // namespace daw::io::wav
