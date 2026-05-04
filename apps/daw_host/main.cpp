#include <iostream>

#include "daw_core/Engine.hpp"

int main() {
    daw_core::Project project;
    project.name = "scratch";

    daw_core::Engine engine(project);
    auto buffer = engine.renderStub(2);

    std::cout << "render_stub_samples=" << buffer.size() << '\n';
    return 0;
}
