#ifndef libslic3r_FuzzySkin_hpp_
#define libslic3r_FuzzySkin_hpp_

#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/PerimeterGenerator.hpp"

namespace Slic3r::Feature::FuzzySkin {

void fuzzy_polyline(Points& poly, bool closed, coordf_t slice_z, const FuzzySkinConfig& cfg);

typedef std::pair<Vec2d, coord_t> FuzzyArtJunction;
typedef std::pair<Point, double> ConnectionPoint;
std::vector<FuzzyArtJunction> get_fuzzy_art_shape(const FuzzySkinConfig& cfg, coord_t& line_width);
void fuzzy_art_line(Arachne::ExtrusionJunctions& ext_lines, coordf_t slice_z, const FuzzySkinConfig& cfg);

void fuzzy_extrusion_line(Arachne::ExtrusionJunctions& ext_lines, coordf_t slice_z, const FuzzySkinConfig& cfg);

void group_region_by_fuzzify(PerimeterGenerator& g);

bool should_fuzzify(const FuzzySkinConfig& config, int layer_id, size_t loop_idx, bool is_contour);

Polygon apply_fuzzy_skin(const Polygon& polygon, const PerimeterGenerator& perimeter_generator, size_t loop_idx, bool is_contour);
void    apply_fuzzy_skin(Arachne::ExtrusionLine* extrusion, const PerimeterGenerator& perimeter_generator, bool is_contour);

} // namespace Slic3r::Feature::FuzzySkin

#endif // libslic3r_FuzzySkin_hpp_
