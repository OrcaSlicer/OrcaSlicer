#pragma once

// Conversion between the pipeline's triangle soup and the indexed mesh used elsewhere. The pipeline
// stays on soup because each stage welds on its own grid, and those differences are load-bearing.

#include "TextureBakeIndex.hpp"
#include "../TriangleMesh.hpp"

namespace Slic3r {
namespace TextureBake {

// `face_excluded`: one entry per input triangle, becoming the soup's per-corner exclusion weight.
TriSoup to_soup(const indexed_triangle_set &its, const std::vector<uint8_t> &face_excluded = {});

// Welds at the geometry grid.
indexed_triangle_set to_indexed_triangle_set(const TriSoup &soup);

} // namespace TextureBake
} // namespace Slic3r
