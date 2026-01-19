#ifndef SRC_LIBSLIC3R_ALGORITHM_REGION_EXPANSION_HPP_
#define SRC_LIBSLIC3R_ALGORITHM_REGION_EXPANSION_HPP_

#include <cstdint>
#include <libslic3r/Point.hpp>
#include <libslic3r/Polygon.hpp>
#include <libslic3r/ExPolygon.hpp>

namespace Slic3r {
namespace Algorithm {

struct RegionExpansionParameters
{
    // Initial expansion of src to make the source regions intersect with boundary regions just a bit.
    float                  tiny_expansion;
    // How much to inflate the seed lines to produce the first wave area.
    float                  initial_step;
    // How much to inflate the first wave area and the successive wave areas in each step.
    float                  other_step;
    // Number of inflate steps after the initial step.
    size_t                 num_other_steps;
    // Maximum inflation of seed contours over the boundary. Used to trim boundary to speed up
    // clipping during wave propagation.
    float                  max_inflation;

    // Accuracy of the offsetter for wave propagation.
    double                 arc_tolerance;
    double                 shortest_edge_length;

    static RegionExpansionParameters build(
        // Scaled expansion value
        float                full_expansion,
        // Expand by waves of expansion_step size (expansion_step is scaled).
        float                expansion_step,
        // Don't take more than max_nr_steps for small expansion_step.
        size_t               max_nr_expansion_steps);
};

struct WaveSeed {
    uint32_t src;
    uint32_t boundary;
    Points   path;
};
using WaveSeeds = std::vector<WaveSeed>;

inline bool lower_by_boundary_and_src(const WaveSeed &l, const WaveSeed &r)
{
    return l.boundary < r.boundary || (l.boundary == r.boundary && l.src < r.src);
}

inline bool lower_by_src_and_boundary(const WaveSeed &l, const WaveSeed &r)
{
    return l.src < r.src || (l.src == r.src && l.boundary < r.boundary);
}

// Expand src slightly outwards to intersect boundaries, trim the offsetted src polylines by the boundaries.
// Return the trimmed paths annotated with their origin (source of the path, index of the boundary region).
WaveSeeds wave_seeds(
    // Source regions that are supposed to touch the boundary.
    const ExPolygons      &src,
    // Boundaries of source regions touching the "boundary" regions will be expanded into the "boundary" region.
    const ExPolygons      &boundary,
    // Initial expansion of src to make the source regions intersect with boundary regions just a bit.
    float                  tiny_expansion,
    bool                   sorted);

struct RegionExpansion
{
    Polygon     polygon;
    uint32_t    src_id;
    uint32_t    boundary_id;
};

std::vector<RegionExpansion> propagate_waves(const WaveSeeds &seeds, const ExPolygons &boundary, const RegionExpansionParameters &params);
std::vector<RegionExpansion> propagate_waves(const ExPolygons &src, const ExPolygons &boundary, const RegionExpansionParameters &params);

std::vector<RegionExpansion> propagate_waves(const ExPolygons &src, const ExPolygons &boundary,
    // Scaled expansion value
    float expansion, 
    // Expand by waves of expansion_step size (expansion_step is scaled).
    float expansion_step,
    // Don't take more than max_nr_steps for small expansion_step.
    size_t max_nr_steps);

struct RegionExpansionEx
{
    ExPolygon   expolygon;
    uint32_t    src_id;
    uint32_t    boundary_id;
};

std::vector<RegionExpansionEx> propagate_waves_ex(const WaveSeeds &seeds, const ExPolygons &boundary, const RegionExpansionParameters &params);

std::vector<RegionExpansionEx> propagate_waves_ex(const ExPolygons &src, const ExPolygons &boundary,
    // Scaled expansion value
    float expansion, 
    // Expand by waves of expansion_step size (expansion_step is scaled).
    float expansion_step,
    // Don't take more than max_nr_steps for small expansion_step.
    size_t max_nr_steps);

std::vector<Polygons> expand_expolygons(const ExPolygons &src, const ExPolygons &boundary,
    // Scaled expansion value
    float expansion, 
    // Expand by waves of expansion_step size (expansion_step is scaled).
    float expansion_step,
    // Don't take more than max_nr_steps for small expansion_step.
    size_t max_nr_steps);

// Merge src with expansions, return the merged expolygons.
std::vector<ExPolygon> merge_expansions_into_expolygons(ExPolygons &&src, std::vector<RegionExpansion> &&expanded);

std::vector<ExPolygon> expand_merge_expolygons(ExPolygons &&src, const ExPolygons &boundary, const RegionExpansionParameters &params);

} // Algorithm
} // Slic3r

#endif /* SRC_LIBSLIC3R_ALGORITHM_REGION_EXPANSION_HPP_ */
