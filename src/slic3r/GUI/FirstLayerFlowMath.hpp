#pragma once
#include <array>
#include <cmath>
#include <stdexcept>

namespace FirstLayerFlow {
struct Patch { int number; double displayed_target; double multiplier; };
// Pure calculation module: no printer settings are modified.
inline std::array<Patch,11> calculate(double selected_flow) {
    if (!std::isfinite(selected_flow) || selected_flow < 0.50 || selected_flow > 1.50)
        throw std::invalid_argument("Selected filament flow must be finite and between 0.50 and 1.50");
    // For positive inputs, floor(x + 0.5) is decimal half-up rounding.
    const int center_hundredths = static_cast<int>(std::floor(selected_flow * 100.0 + 0.5 + 1e-10));
    constexpr std::array<int,11> offsets{{-1,-2,-3,-4,-5,1,2,3,4,5,0}};
    std::array<Patch,11> patches{};
    for (int i=0;i<11;++i) {
        const double target = (center_hundredths + offsets[i]) / 100.0;
        patches[i] = {i+1, target, i==10 ? 1.0 : target / selected_flow};
    }
    return patches;
}
} // namespace FirstLayerFlow
