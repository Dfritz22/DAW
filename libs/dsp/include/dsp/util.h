#pragma once

namespace daw::dsp {

// dB → linear amplitude. Pure; safe to call from any thread.
float DbToLinear(float db);

} // namespace daw::dsp
