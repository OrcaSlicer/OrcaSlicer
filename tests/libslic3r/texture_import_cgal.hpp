#pragma once

#include <CGAL/AABB_face_graph_triangle_primitive.h>
#include <CGAL/AABB_traits_3.h>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Kd_tree.h>
#include <CGAL/Search_traits_adapter.h>
#include <CGAL/Surface_mesh/Surface_mesh.h>
#include <CGAL/property_map.h>

#include <cstddef>

namespace TextureImportTest {

// The distance-query tree used by ColorUtils.cpp's boundary smoothing. Its
// type name stays the same when CGAL changes its storage for TBB support.
using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
using Mesh = CGAL::Surface_mesh<Kernel::Point_3>;
using Primitive = CGAL::AABB_face_graph_triangle_primitive<Mesh>;
using Traits = CGAL::AABB_traits_3<Kernel, Primitive>;
using PointId = Traits::Point_and_primitive_id;
using SearchTraits = CGAL::Search_traits_adapter<PointId, CGAL::First_of_pair_property_map<PointId>, Traits>;
using SearchTree = CGAL::Kd_tree<SearchTraits>;

// Implemented inside the actual geometry target, using its private definitions.
std::size_t backend_search_tree_size();

} // namespace TextureImportTest
