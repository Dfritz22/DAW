#include "io/wav.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace daw::io::wav {

const wchar_t* Describe(Error e) {
    switch (e) {
        case Error::OpenFailed:          return L"Could not open file.";
        case Error::ReadFailed:          return L"Read error while loading WAV.";
        case Error::WriteFailed:         return L"Write error while saving WAV.";
        case Error::TooSmall:            return L"File too small to be a WAV.";
        case Error::NotRiffWave:         return L"Only RIFF/WAVE files are supported.";
        case Error::MissingChunks:       return L"Missing fmt/data chunks.";
        case Error::UnsupportedChannels: return L"Only mono/stereo WAV files are currently supported.";
        case Error::UnsupportedFormat:   return L"Supported WAV formats: PCM 16/24/32-bit or IEEE float 32-bit.";
        case Error::InvalidArguments:    return L"Invalid arguments to WAV codec.";
    }
    return L"Unknown WAV error.";
}

// ── ParseStereo ─────────────────────────────────────────────────────────────

base::Result<StereoBuffer, Error>
ParseStereo(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 44) return base::Err{Error::TooSmall};
    if (std::memcmp(bytes.data(),     "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return base::Err{Error::NotRiffWave};
    }

    auto readU16 = [&bytes](size_t off) -> std::uint16_t {
        return static_cast<std::uint16_t>(bytes[off] |
                                          (bytes[off + 1] << 8));
    };
    auto readU32 = [&bytes](size_t off) -> std::uint32_t {
        return static_cast<std::uint32_t>(
            bytes[off] |
            (bytes[off + 1] << 8) |
            (bytes[off + 2] << 16) |
            (bytes[off + 3] << 24));
    };

    bool          hasFmt = false, hasData = false;
    std::uint16_t fmtAudio = 0, fmtChannels = 0, fmtBits = 0;
    std::uint32_t fmtSampleRate = 0;
    size_t        dataOffset = 0;
    std::uint32_t dataSize = 0;

    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const char*         id        = reinterpret_cast<const char*>(bytes.data() + pos);
        const std::uint32_t chunkSize = readU32(pos + 4);
        const size_t        chunkData = pos + 8;
        if (chunkData + chunkSize > bytes.size()) break;

        if (std::memcmp(id, "fmt ", 4) == 0 && chunkSize >= 16) {
            fmtAudio      = readU16(chunkData + 0);
            fmtChannels   = readU16(chunkData + 2);
            fmtSampleRate = readU32(chunkData + 4);
            fmtBits       = readU16(chunkData + 14);
            hasFmt        = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataOffset = chunkData;
            dataSize   = chunkSize;
            hasData    = true;
        }
        // Chunks are word-aligned: skip the size + a possible pad byte.
        pos = chunkData + chunkSize + (chunkSize % 2);
    }

    if (!hasFmt || !hasData)               return base::Err{Error::MissingChunks};
    if (fmtChannels != 1 && fmtChannels != 2) return base::Err{Error::UnsupportedChannels};

    const bool isPcm             = (fmtAudio == 1);
    const bool isFloat           = (fmtAudio == 3);
    const bool supportedPcmBits  = (fmtBits == 16 || fmtBits == 24 || fmtBits == 32);
    const bool supportedFltBits  = (fmtBits == 32);
    if ((!isPcm   || !supportedPcmBits) &&
        (!isFloat || !supportedFltBits)) {
        return base::Err{Error::UnsupportedFormat};
    }

    const size_t bytesPerSample = static_cast<size_t>(fmtBits / 8);
    if (bytesPerSample == 0) return base::Err{Error::UnsupportedFormat};

    const size_t sampleCount = dataSize / bytesPerSample;
    const size_t frameCount  = sampleCount / fmtChannels;

    auto decodeSample = [&](const std::uint8_t* p) -> float {
        if (isPcm && fmtBits == 16) {
            const std::int16_t v = static_cast<std::int16_t>(
                static_cast<std::uint16_t>(p[0] | (p[1] << 8)));
            return static_cast<float>(v) / 32768.0f;
        }
        if (isPcm && fmtBits == 24) {
            std::int32_t v = static_cast<std::int32_t>(
                p[0] | (p[1] << 8) | (p[2] << 16));
            // Sign-extend the top byte.
            if ((v & 0x00800000) != 0) v |= ~0x00FFFFFF;
            return static_cast<float>(v) / 8388608.0f;
        }
        if (isPcm && fmtBits == 32) {
            const std::int32_t v = static_cast<std::int32_t>(
                static_cast<std::uint32_t>(p[0])         |
                (static_cast<std::uint32_t>(p[1]) << 8)  |
                (static_cast<std::uint32_t>(p[2]) << 16) |
                (static_cast<std::uint32_t>(p[3]) << 24));
            return static_cast<float>(static_cast<double>(v) / 2147483648.0);
        }
        if (isFloat && fmtBits == 32) {
            float v = 0.0f;
            std::memcpy(&v, p, sizeof(float));
            return std::clamp(v, -1.0f, 1.0f);
        }
        return 0.0f;
    };

    StereoBuffer out;
    out.sampleRate  = static_cast<int>(fmtSampleRate);
    out.frames      = static_cast<std::uint32_t>(frameCount);
    out.interleaved.assign(frameCount * 2, 0.0f);
    for (size_t i = 0; i < frameCount; ++i) {
        const std::uint8_t* frame = bytes.data() + dataOffset + i * fmtChannels * bytesPerSample;
        const float l = decodeSample(frame);
        const float r = (fmtChannels == 2) ? decodeSample(frame + bytesPerSample) : l;
        out.interleaved[i * 2]     = l;
        out.interleaved[i * 2 + 1] = r;
    }
    return out;
}

// ── EncodePcm16Stereo ───────────────────────────────────────────────────────

base::Result<std::vector<std::uint8_t>, Error>
EncodePcm16Stereo(const std::vector<float>& interleaved, int sampleRate) {
    if (sampleRate <= 0 || interleaved.empty() || (interleaved.size() % 2 != 0)) {
        return base::Err{Error::InvalidArguments};
    }

    const std::uint32_t frames    = static_cast<std::uint32_t>(interleaved.size() / 2);
    const std::uint32_t dataBytes = frames * 2u * static_cast<std::uint32_t>(sizeof(std::int16_t));

    std::vector<std::uint8_t> out;
    out.reserve(static_cast<size_t>(44u + dataBytes));

    auto putU16 = [&out](std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>( v        & 0xFF));
        out.push_back(static_cast<std::uint8_t>((v >> 8)  & 0xFF));
    };
    auto putU32 = [&out](std::uint32_t v) {
        out.push_back(static_cast<std::uint8_t>( v         & 0xFF));
        out.push_back(static_cast<std::uint8_t>((v >> 8)   & 0xFF));
        out.push_back(static_cast<std::uint8_t>((v >> 16)  & 0xFF));
        out.push_back(static_cast<std::uint8_t>((v >> 24)  & 0xFF));
    };
    auto putBytes = [&out](const char* s, size_t n) {
        for (size_t i = 0; i < n; ++i) out.push_back(static_cast<std::uint8_t>(s[i]));
    };

    putBytes("RIFF", 4);
    putU32(36u + dataBytes);
    putBytes("WAVE", 4);

    putBytes("fmt ", 4);
    putU32(16u);                                         // PCM fmt chunk size
    putU16(1u);                                          // PCM
    putU16(2u);                                          // channels
    putU32(static_cast<std::uint32_t>(sampleRate));
    putU32(static_cast<std::uint32_t>(sampleRate) * 2u * static_cast<std::uint32_t>(sizeof(std::int16_t))); // byte rate
    putU16(static_cast<std::uint16_t>(2u * sizeof(std::int16_t))); // block align
    putU16(16u);                                         // bits per sample

    putBytes("data", 4);
    putU32(dataBytes);

    for (float s : interleaved) {
        const float        clamped = std::clamp(s, -1.0f, 1.0f);
        const std::int16_t v       = static_cast<std::int16_t>(std::lrint(clamped * 32767.0f));
        putU16(static_cast<std::uint16_t>(v));
    }

    return out;
}

// ── File wrappers ───────────────────────────────────────────────────────────

base::Result<StereoBuffer, Error>
LoadStereoFromFile(const std::wstring& path) {
    if (path.empty()) return base::Err{Error::InvalidArguments};
    std::ifstream f(path, std::ios::binary);
    if (!f) return base::Err{Error::OpenFailed};

    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0, std::ios::beg);
    if (size < 0) return base::Err{Error::ReadFailed};

    std::vector<std::uint8_t> bytes(static_cast<size_t>(size));
    if (size > 0) {
        f.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!f) return base::Err{Error::ReadFailed};
    }
    return ParseStereo(bytes);
}

base::Result<void, Error>
WritePcm16StereoToFile(const std::wstring& path,
                       const std::vector<float>& interleaved,
                       int sampleRate) {
    if (path.empty()) return base::Err{Error::InvalidArguments};
    auto encoded = EncodePcm16Stereo(interleaved, sampleRate);
    if (!encoded) return base::Err{encoded.error()};

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return base::Err{Error::OpenFailed};
    const auto& bytes = encoded.value();
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out) return base::Err{Error::WriteFailed};
    }
    return {};
}

} // namespace daw::io::wav
