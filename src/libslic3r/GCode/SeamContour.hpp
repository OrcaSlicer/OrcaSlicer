#pragma once

#include "libslic3r/Point.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include <cstddef>
#include <functional>
#include <map>
#include <tuple>
#include <vector>

namespace Slic3r::SeamContour {

// Geometry is immutable during string construction; finalized state is deliberately absent.
struct Contour {
    std::vector<Vec2d> points;
    bool hole = false;
    bool reliable = true; // Only closed extrusion loops provide a trustworthy boundary role.
    double width = 0.4;
    double nozzle = 0.4;
    Vec2d min = Vec2d::Zero();
    Vec2d max = Vec2d::Zero();
};
using Layers = std::vector<std::vector<Contour>>;

struct Options {
    double bounds_nozzle_factor = 1.25; // One nozzle diameter plus a quarter-diameter reserve.
    double radius_width_factor = 4.0;  // Preserve the existing continuation radius.
    double noise_width_factor = 0.125;
    double normal_cosine = 0.5;        // Up to 60 degrees for geometric admissibility.
    double dominance_width_factor = 0.25; // Distance advantage required to discard another contour.
    double dominance_angle_degrees = 5.0; // Normal agreement must also improve by this margin.
};

enum class Outcome { Unique, None, Multiple };
enum class Method { Bounds, Local };
struct Result {
    Outcome outcome = Outcome::None;
    Method method = Method::Local;
    std::vector<size_t> contours; // Empty means no geometric restriction on the legacy search.
    bool uncertain = false; // Reverse checks must propagate unknown geometry, not reject one candidate.
};

class Resolver {
public:
    explicit Resolver(Layers layers, Options options = {}, std::function<void()> throw_if_canceled = {});
    Result resolve(size_t layer, size_t contour, size_t vertex, int direction, double radius = -1.0) const;
private:
    // Alternative string starts reuse the same geometric queries in both directions.
    using Key = std::tuple<size_t, size_t, size_t, int, double>;
    Layers m_layers;
    std::vector<AABBTreeIndirect::Tree2d> m_indices;
    Options m_options;
    mutable std::map<Key, Result> m_cache;
    std::vector<size_t> query(size_t layer, const Vec2d &point, double reach) const;
    bool in_material(size_t layer, const Vec2d &point) const;
    bool near_boundary(size_t layer, const Vec2d &point, double noise) const;
    Result select(size_t layer, size_t contour, const Vec2d &point, int direction, double radius,
                  bool check_reverse) const;
};

} // namespace Slic3r::SeamContour
