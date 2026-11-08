///|/ Wave overhangs algorithm: Janis A. Andersons (andersonsjanis).
///|/ Builds on arc-overhang algorithm by Steven McCulloch (stmcculloch).
///|/ PrusaSlicer integration: Steven McCulloch.
///|/ Port to OrcaSlicer: Dennis Klappe (dennisklappe).
///|/
///|/ Released under the terms of the AGPLv3 or higher.
///|/
#ifndef slic3r_WaveOverhangs_AndersonsGenerator_hpp_
#define slic3r_WaveOverhangs_AndersonsGenerator_hpp_

#include "IGenerator.hpp"

namespace Slic3r::WaveOverhangs {

class AndersonsGenerator : public IGenerator {
public:
    GenerateResult generate(const ExPolygons   &overhang_area,
                            const Polygons     &lower_slices_polygons,
                            const CommonParams &params) override;
    const char *name() const override { return "andersons"; }
};

} // namespace Slic3r::WaveOverhangs

#endif
