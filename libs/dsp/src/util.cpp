#include "dsp/util.h"

#include <cmath>

namespace daw::dsp {

float DbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

} // namespace daw::dsp
