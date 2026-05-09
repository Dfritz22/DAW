#include "wav_io.h"

#include "core/state.h"
#include <fstream>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <cmath>

bool IoLoadWavStereo(const std::wstring& path, LoadedAudio* out, std::wstring* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        *error = L"Could not open file.";
        return false;
    }

    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0, std::ios::beg);
    if (size < 44) {
        *error = L"File too small to be a WAV.";
        return false;
    }

    std::vector<std::uint8_t> bytes(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(bytes.data()), size);

    auto readU16 = [&bytes](size_t off) -> std::uint16_t {
        return static_cast<std::uint16_t>(bytes[off] | (bytes[off + 1] << 8));
    };
    auto readU32 = [&bytes](size_t off) -> std::uint32_t {
        return static_cast<std::uint32_t>(
            bytes[off] |
            (bytes[off + 1] << 8) |
            (bytes[off + 2] << 16) |
            (bytes[off + 3] << 24)
        );
    };

    if (std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        *error = L"Only RIFF/WAVE files are supported.";
        return false;
    }

    bool hasFmt = false;
    bool hasData = false;
    std::uint16_t fmtAudio = 0;
    std::uint16_t fmtChannels = 0;
    std::uint32_t fmtSampleRate = 0;
    std::uint16_t fmtBits = 0;
    size_t dataOffset = 0;
    std::uint32_t dataSize = 0;

    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const char* id = reinterpret_cast<const char*>(bytes.data() + pos);
        const std::uint32_t chunkSize = readU32(pos + 4);
        const size_t chunkData = pos + 8;
        if (chunkData + chunkSize > bytes.size()) {
            break;
        }

        if (std::memcmp(id, "fmt ", 4) == 0 && chunkSize >= 16) {
            fmtAudio = readU16(chunkData + 0);
            fmtChannels = readU16(chunkData + 2);
            fmtSampleRate = readU32(chunkData + 4);
            fmtBits = readU16(chunkData + 14);
            hasFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataOffset = chunkData;
            dataSize = chunkSize;
            hasData = true;
        }

        pos = chunkData + chunkSize + (chunkSize % 2);
    }

    if (!hasFmt || !hasData) {
        *error = L"Missing fmt/data chunks.";
        return false;
    }
    if (fmtChannels != 1 && fmtChannels != 2) {
        *error = L"Only mono/stereo WAV files are currently supported.";
        return false;
    }

    const bool isPcm = (fmtAudio == 1);
    const bool isFloat = (fmtAudio == 3);
    const bool supportedPcmBits = (fmtBits == 16 || fmtBits == 24 || fmtBits == 32);
    const bool supportedFloatBits = (fmtBits == 32);

    if ((!isPcm || !supportedPcmBits) && (!isFloat || !supportedFloatBits)) {
        *error = L"Supported WAV formats: PCM 16/24/32-bit or IEEE float 32-bit.";
        return false;
    }

    const size_t bytesPerSample = static_cast<size_t>(fmtBits / 8);
    if (bytesPerSample == 0) {
        *error = L"Invalid bit depth in WAV file.";
        return false;
    }

    const size_t sampleCount = dataSize / bytesPerSample;
    const size_t frameCount = sampleCount / fmtChannels;
    std::vector<float> stereo(frameCount * 2, 0.0f);

    auto decodeSample = [&](const std::uint8_t* p) -> float {
        if (isPcm && fmtBits == 16) {
            const std::int16_t v = static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0] | (p[1] << 8)));
            return static_cast<float>(v) / 32768.0f;
        }
        if (isPcm && fmtBits == 24) {
            std::int32_t v = static_cast<std::int32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
            if ((v & 0x00800000) != 0) {
                v |= ~0x00FFFFFF;
            }
            return static_cast<float>(v) / 8388608.0f;
        }
        if (isPcm && fmtBits == 32) {
            const std::int32_t v = static_cast<std::int32_t>(
                static_cast<std::uint32_t>(p[0]) |
                (static_cast<std::uint32_t>(p[1]) << 8) |
                (static_cast<std::uint32_t>(p[2]) << 16) |
                (static_cast<std::uint32_t>(p[3]) << 24)
            );
            return static_cast<float>(static_cast<double>(v) / 2147483648.0);
        }
        if (isFloat && fmtBits == 32) {
            float v = 0.0f;
            std::memcpy(&v, p, sizeof(float));
            return std::clamp(v, -1.0f, 1.0f);
        }
        return 0.0f;
    };

    for (size_t i = 0; i < frameCount; ++i) {
        const std::uint8_t* frame = bytes.data() + dataOffset + i * fmtChannels * bytesPerSample;
        const float l = decodeSample(frame);
        const float r = (fmtChannels == 2) ? decodeSample(frame + bytesPerSample) : l;
        stereo[i * 2] = l;
        stereo[i * 2 + 1] = r;
    }

    out->sourcePath = path;
    out->displayName = std::filesystem::path(path).stem().wstring();
    out->sampleRate = static_cast<int>(fmtSampleRate);
    out->frames = static_cast<std::uint32_t>(frameCount);
    out->stereo = std::move(stereo);
    return true;
}

bool IoWriteWavPcm16Stereo(const std::wstring& path, const std::vector<float>& stereo, int sampleRate) {
    if (sampleRate <= 0 || stereo.empty() || (stereo.size() % 2 != 0)) {
        return false;
    }

    const std::uint32_t frames = static_cast<std::uint32_t>(stereo.size() / 2);
    const std::uint32_t dataBytes = frames * 2 * sizeof(std::int16_t);

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    auto writeU16 = [&out](std::uint16_t v) {
        out.put(static_cast<char>(v & 0xFF));
        out.put(static_cast<char>((v >> 8) & 0xFF));
    };
    auto writeU32 = [&out](std::uint32_t v) {
        out.put(static_cast<char>(v & 0xFF));
        out.put(static_cast<char>((v >> 8) & 0xFF));
        out.put(static_cast<char>((v >> 16) & 0xFF));
        out.put(static_cast<char>((v >> 24) & 0xFF));
    };

    out.write("RIFF", 4);
    writeU32(36 + dataBytes);
    out.write("WAVE", 4);

    out.write("fmt ", 4);
    writeU32(16);
    writeU16(1);
    writeU16(2);
    writeU32(static_cast<std::uint32_t>(sampleRate));
    writeU32(static_cast<std::uint32_t>(sampleRate * 2 * sizeof(std::int16_t)));
    writeU16(2 * sizeof(std::int16_t));
    writeU16(16);

    out.write("data", 4);
    writeU32(dataBytes);

    for (float s : stereo) {
        const float clamped = std::clamp(s, -1.0f, 1.0f);
        const std::int16_t v = static_cast<std::int16_t>(std::lrint(clamped * 32767.0f));
        writeU16(static_cast<std::uint16_t>(v));
    }

    return true;
}
