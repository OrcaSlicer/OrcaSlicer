#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <boost/multiprecision/cpp_int.hpp>

namespace Slic3r::Gpu {

// GPU kernels use signed 64-bit values. Production dispatch checks the raw
// coordinate arithmetic and destination representation before submission.
// The independent host oracle has room for products of full-range int64
// differences and their sums, without mirroring device overflow.
using Coord = int64_t;
using WideCoord = boost::multiprecision::int256_t;

// Legacy tile helpers are not the production dispatch contract; callers use
// their own current application coordinate scale and checked raw coordinates.
constexpr Coord coordinate_scale = 1'000'000; // one million fixed-point units per millimetre.
constexpr Coord max_tile_radius = 2'000'000;  // 2 mm from a tile origin.

struct Point {
    Coord x { 0 };
    Coord y { 0 };
};

struct Segment {
    Point a;
    Point b;
};

inline WideCoord orient2d(const Point& a, const Point& b, const Point& c)
{
    const WideCoord abx = WideCoord(b.x) - a.x;
    const WideCoord aby = WideCoord(b.y) - a.y;
    const WideCoord acx = WideCoord(c.x) - a.x;
    const WideCoord acy = WideCoord(c.y) - a.y;
    return abx * acy - aby * acx;
}

inline bool fits_in_tile(const Point& point, const Point& tile_origin)
{
    const WideCoord dx = WideCoord(point.x) - tile_origin.x;
    const WideCoord dy = WideCoord(point.y) - tile_origin.y;
    return dx >= -max_tile_radius && dx <= max_tile_radius &&
           dy >= -max_tile_radius && dy <= max_tile_radius;
}

inline bool fits_in_tile(const Segment& segment, const Point& tile_origin)
{
    return fits_in_tile(segment.a, tile_origin) && fits_in_tile(segment.b, tile_origin);
}

// Stable IDs, rather than GPU workgroup order, are the tie-breaker for every
// wall or infill segment emitted by a compute stage.
struct StableSegment {
    Segment segment;
    uint64_t stable_id { 0 };
};

struct VerticalIntersectionRequest {
    Segment segment;
    Coord scan_x { 0 };
    uint64_t stable_id { 0 };
};

struct VerticalIntersection {
    Coord numerator { 0 };
    Coord denominator { 1 };
    uint64_t stable_id { 0 };
    bool valid { false };
};

struct VerticalIntersectionBatch {
    bool dispatched { false };
    std::string diagnostic;
    double gpu_elapsed_ms { -1.0 };
    double host_elapsed_ms { 0.0 };
    uint32_t queue_submissions { 0 };
    std::vector<VerticalIntersection> intersections;
};

struct TreeContourRequest {
    Segment segment;
    uint64_t stable_id { 0 };
};

struct TreeContourBatch {
    bool dispatched { false };
    std::string diagnostic;
    std::vector<uint8_t> may_intersect;
};

} // namespace Slic3r::Gpu
