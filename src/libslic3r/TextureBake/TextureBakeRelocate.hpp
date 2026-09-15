#pragma once

// Tangential relocation: slide vertices along the surface so triangle edges land on the height map's
// own edges, before any displacement happens.
//
// Displacement moves vertices along the normal only, so a step in the height map - the wall of a
// mortar groove, the rim of an embossed shape - is reproduced wherever the triangle grid happens to
// fall, as a staircase quantised to triangle boundaries. Refining further only makes the steps
// smaller; it never straightens them, because the edge in the image still does not coincide with any
// edge in the mesh.
//
// This pass fixes the cause rather than the symptom. For a vertex sitting near a step it takes a
// Newton step onto the contour: with h the sampled height and g its tangential gradient, the move
//
//     d = -(h - target) * g / |g|^2
//
// lands on the level set h = target to first order. The ring of vertices nearest each step therefore
// snaps onto it, the triangle edges between them follow the step, and the displaced result has a
// straight wall instead of a sawtooth.
//
// Vertices in flat regions have no gradient to speak of and are left alone, so the pass costs nothing
// where there is nothing to align.

#include <cstdint>
#include <functional>
#include <vector>

#include "TextureBakeDisplace.hpp"
#include "TextureBakeIndex.hpp"

namespace Slic3r {
namespace TextureBake {

struct RelocateSettings
{
    // Passes. Each is a Newton step, so a couple converge for vertices that start reasonably close;
    // more mainly helps ones that begin further away.
    int iterations = 3;

    // How far a vertex may move in one pass, as a fraction of the mean length of its incident edges.
    // Below a half it cannot pass a neighbour, which is what keeps the triangulation valid without
    // needing a full topological check.
    double max_move_fraction = 0.35;

    // A vertex is only pulled when the height varies enough across its own footprint to mean
    // something - as a fraction of the height range over the whole patch. Below this the gradient is
    // noise, and chasing it would scramble flat regions.
    double min_gradient_fraction = 0.05;

    // The level to snap onto, as a position in the sampled height range: 0.5 is midway between the
    // lowest and highest point of the relief, which is where the wall of a step is steepest.
    double contour_level = 0.5;

    // Sampling offset for the finite-difference gradient, as a fraction of the local edge length.
    double gradient_step_fraction = 0.25;
};

struct RelocateResult
{
    TriSoup geometry;
    size_t  moved = 0;      // positions that were relocated at least once
    size_t  rejected = 0;   // moves refused because a triangle would have inverted
};

// `locked`, when non-empty, has one entry per input triangle; a vertex touching a locked triangle is
// never moved, so an excluded region keeps its exact vertex positions.
RelocateResult relocate_to_contours(const TriSoup &geometry, const HeightSampleFn &sample,
                                    const RelocateSettings &settings = {},
                                    const std::vector<uint8_t> &locked = {});

} // namespace TextureBake
} // namespace Slic3r
