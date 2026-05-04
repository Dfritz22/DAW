#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace daw_core {

struct TrackClip {
    std::string trackName;
    std::string sourceFile;
    std::int32_t startBar {1};
    std::int32_t startBeat {1};
    float gainDb {0.0f};
    float pan {0.0f};
    bool muted {false};
    bool solo {false};
};

struct Project {
    std::string name;
    std::int32_t sampleRate {48000};
    float tempoBpm {120.0f};
    std::int32_t timeSignatureNum {4};
    std::int32_t timeSignatureDen {4};
    std::vector<TrackClip> clips;
};

} // namespace daw_core
