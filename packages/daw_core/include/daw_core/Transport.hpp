#pragma once

#include <cstdint>

namespace daw_core {

class Transport {
public:
    void play();
    void stop();
    void record();
    void rewind();

    [[nodiscard]] bool isPlaying() const { return isPlaying_; }
    [[nodiscard]] bool isRecording() const { return isRecording_; }
    [[nodiscard]] std::int64_t playheadSample() const { return playheadSample_; }

private:
    bool isPlaying_ {false};
    bool isRecording_ {false};
    std::int64_t playheadSample_ {0};
};

} // namespace daw_core
