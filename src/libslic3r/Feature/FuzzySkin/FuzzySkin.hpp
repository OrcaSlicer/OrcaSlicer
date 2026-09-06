#ifndef libslic3r_FuzzySkin_hpp_
#define libslic3r_FuzzySkin_hpp_

#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/PerimeterGenerator.hpp"

#include <memory>

// Forward declared rather than including libnoise here: this header is pulled in
// widely, and only callers of get_noise_module() need the complete type.
namespace noise { namespace module { class Module; } }

namespace Slic3r::Feature::FuzzySkin {

// Shared noise field. Sampling the same module for top surfaces as for the walls is
// what makes the texture continuous where the two meet.
std::unique_ptr<noise::module::Module> get_noise_module(const FuzzySkinConfig& cfg);

void fuzzy_polyline(Points& poly, bool closed, coordf_t slice_z, const FuzzySkinConfig& cfg);

void fuzzy_extrusion_line(Arachne::ExtrusionJunctions& ext_lines, coordf_t slice_z, const FuzzySkinConfig& cfg, bool closed = true);

void group_region_by_fuzzify(PerimeterGenerator& g);

bool should_fuzzify(const FuzzySkinConfig& config, int layer_id, size_t loop_idx, bool is_contour);

Polygon apply_fuzzy_skin(const Polygon& polygon, const PerimeterGenerator& perimeter_generator, size_t loop_idx, bool is_contour);
void    apply_fuzzy_skin(Arachne::ExtrusionLine* extrusion, const PerimeterGenerator& perimeter_generator, bool is_contour, bool closed = true);

} // namespace Slic3r::Feature::FuzzySkin

#endif // libslic3r_FuzzySkin_hpp_
