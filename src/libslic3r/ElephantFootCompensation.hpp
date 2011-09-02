#ifndef slic3r_ElephantFootCompensation_hpp_
#define slic3r_ElephantFootCompensation_hpp_

#include "libslic3r.h"
#include "ExPolygon.hpp"
#include <vector>

namespace Slic3r {

class Flow;

ExPolygon  elephant_foot_compensation(const ExPolygon  &input, double min_countour_width, const double compensation);
ExPolygons elephant_foot_compensation(const ExPolygons &input, double min_countour_width, const double compensation);
ExPolygon  elephant_foot_compensation(const ExPolygon  &input, const Flow &external_perimeter_flow, const double compensation);
ExPolygons elephant_foot_compensation(const ExPolygons &input, const Flow &external_perimeter_flow, const double compensation);

} // Slic3r

#endif /* slic3r_ElephantFootCompensation_hpp_ */
