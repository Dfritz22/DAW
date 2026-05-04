#pragma once

#include <vector>

#include "daw_core/Project.hpp"
#include "daw_core/Timeline.hpp"
#include "daw_core/Transport.hpp"

namespace daw_core {

class Engine {
public:
    explicit Engine(Project project);

    [[nodiscard]] const Project& project() const { return project_; }
    [[nodiscard]] Transport& transport() { return transport_; }
    [[nodiscard]] const Timeline& timeline() const { return timeline_; }

    // Stub renderer: returns silent interleaved stereo audio [L, R, L, R, ...]
    [[nodiscard]] std::vector<float> renderStub(int bars = 4) const;

private:
    Project project_;
    Timeline timeline_;
    Transport transport_;
};

} // namespace daw_core
