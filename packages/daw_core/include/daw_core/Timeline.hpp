#pragma once

#include <cstdint>

namespace daw_core {

class Timeline {
public:
    Timeline(float tempoBpm, std::int32_t sampleRate, std::int32_t beatsPerBar = 4);

    float secondsPerBeat() const;
    std::int64_t samplesPerBeat() const;
    std::int64_t barsBeatsToSample(std::int32_t bar, std::int32_t beat = 1) const;

private:
    float tempoBpm_;
    std::int32_t sampleRate_;
    std::int32_t beatsPerBar_;
};

} // namespace daw_core
