#include <catch2/catch_all.hpp>

#include "libslic3r/Gpu/GpuExactGeometry.hpp"
#include "libslic3r/Gpu/VulkanSlicer.hpp"

#include <limits>

using namespace Slic3r::Gpu;

TEST_CASE("GPU exact geometry uses a wider host orientation reference", "[Gpu][Geometry]")
{
    // This product cannot fit in a signed 64-bit intermediate. It proves the
    // CPU acceptance gate cannot accidentally mirror an overflowing shader
    // calculation when it validates a tile candidate.
    constexpr Coord extent = 1'000'000'000'000;
    const Point a { 0, 0 };
    const Point b { extent, 0 };
    const Point c { 0, extent };

    const WideCoord orientation = orient2d(a, b, c);
    REQUIRE(orientation > std::numeric_limits<Coord>::max());
}

TEST_CASE("GPU exact geometry enforces the tile dispatch range", "[Gpu][Geometry]")
{
    const Point origin { 30 * coordinate_scale, 40 * coordinate_scale };
    const Segment eligible {
        { origin.x - max_tile_radius, origin.y + max_tile_radius },
        { origin.x + max_tile_radius, origin.y - max_tile_radius }
    };
    const Segment out_of_range {
        { origin.x - max_tile_radius - 1, origin.y },
        { origin.x, origin.y }
    };

    REQUIRE(fits_in_tile(eligible, origin));
    REQUIRE_FALSE(fits_in_tile(out_of_range, origin));
}

TEST_CASE("GPU deterministic geometry requires a compute queue and int64 shaders", "[Gpu][Vulkan]")
{
    VulkanDeviceInfo device;

    REQUIRE_FALSE(VulkanSlicerBackend::supports_deterministic_geometry(device));
    device.compute_queue = true;
    REQUIRE_FALSE(VulkanSlicerBackend::supports_deterministic_geometry(device));
    device.shader_int64 = true;
    REQUIRE(VulkanSlicerBackend::supports_deterministic_geometry(device));
}
