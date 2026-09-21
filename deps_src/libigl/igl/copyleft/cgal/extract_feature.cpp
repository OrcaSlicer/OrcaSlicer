// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

#include "extract_feature.h"
#include "../../unique_edge_map.h"
#include "../../PI.h"
#include <CGAL/Kernel/global_functions.h>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>

template<
  typename DerivedV,
  typename DerivedF,
  typename Derivedfeature_edges >
IGL_INLINE void igl::copyleft::cgal::extract_feature(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    const double tol,
    Eigen::PlainObjectBase<Derivedfeature_edges>& feature_edges) {

  using IndexType = typename Derivedfeature_edges::Scalar;
  Derivedfeature_edges E, uE;
  Eigen::VectorXi EMAP;
  std::vector<std::vector<IndexType> > uE2E;
  igl::unique_edge_map(F, E, uE, EMAP, uE2E);

  igl::copyleft::cgal::extract_feature(V, F, tol, E, uE, uE2E, feature_edges);
}

template<
  typename DerivedV,
  typename DerivedF,
  typename DeriveduE,
  typename Derivedfeature_edges
  >
IGL_INLINE void igl::copyleft::cgal::extract_feature(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    const double tol,
    const Eigen::MatrixBase<DeriveduE>& uE,
    const std::vector<std::vector<typename DeriveduE::Scalar> >& uE2E,
    Eigen::PlainObjectBase<Derivedfeature_edges>& feature_edges) 
{

  assert(V.cols() == 3);
  assert(F.cols() == 3);
  using Scalar = typename DerivedV::Scalar;
  using IndexType = typename DeriveduE::Scalar;
  using Vertex = Eigen::Matrix<Scalar, 3, 1>;
  using Kernel = typename CGAL::Exact_predicates_exact_constructions_kernel;
  using Point = typename Kernel::Point_3;

  const size_t num_unique_edges = uE.rows();
  const size_t num_faces = F.rows();
  // NOTE: CGAL's definition of dihedral angle measures the angle between two
  // facets instead of facet normals.
  const double cos_tol = cos(igl::PI - tol);
  std::vector<size_t> result; // Indices into uE

  auto is_non_manifold = [&uE2E](size_t ei) -> bool {
    return uE2E[ei].size() > 2;
  };

  auto is_boundary = [&uE2E](size_t ei) -> bool {
    return uE2E[ei].size() == 1;
  };

  auto opposite_vertex = [&uE, &F](size_t ei, size_t fi) -> IndexType {
    const size_t v0 = uE(ei, 0);
    const size_t v1 = uE(ei, 1);
    for (size_t i=0; i<3; i++) {
      const size_t v = F(fi, i);
      if (v != v0 && v != v1) { return v; }
    }
    throw "Input face must be topologically degenerate!";
  };

  auto is_feature = [&V, &F, &uE, &uE2E, &opposite_vertex, num_faces](
      size_t ei, double cos_tol) -> bool {
    auto adj_faces = uE2E[ei];
    assert(adj_faces.size() == 2);
    const Vertex v0 = V.row(uE(ei, 0));
    const Vertex v1 = V.row(uE(ei, 1));
    const Vertex v2 = V.row(opposite_vertex(ei, adj_faces[0] % num_faces));
    const Vertex v3 = V.row(opposite_vertex(ei, adj_faces[1] % num_faces));
    const Point p0(v0[0], v0[1], v0[2]);
    const Point p1(v1[0], v1[1], v1[2]);
    const Point p2(v2[0], v2[1], v2[2]);
    const Point p3(v3[0], v3[1], v3[2]);
    const auto ori = CGAL::orientation(p0, p1, p2, p3);
    switch (ori) {
      case CGAL::POSITIVE:
        return CGAL::compare_dihedral_angle(p0, p1, p2, p3, cos_tol) ==
          CGAL::SMALLER;
      case CGAL::NEGATIVE:
        return CGAL::compare_dihedral_angle(p0, p1, p3, p2, cos_tol) ==
          CGAL::SMALLER;
      case CGAL::COPLANAR:
        if (!CGAL::collinear(p0, p1, p2) && !CGAL::collinear(p0, p1, p3)) {
          return CGAL::compare_dihedral_angle(p0, p1, p2, p3, cos_tol) ==
            CGAL::SMALLER;
        } else {
          throw "Dihedral angle (and feature edge) is not well defined for"
              " degenerate triangles!";
        }
      default:
        throw "Unknown CGAL orientation";
    }
  };

  for (size_t i=0; i<num_unique_edges; i++) {
    if (is_boundary(i) || is_non_manifold(i) || is_feature(i, cos_tol)) {
      result.push_back(i);
    }
  }

  const size_t num_feature_edges = result.size();
  feature_edges.resize(num_feature_edges, 2);
  for (size_t i=0; i<num_feature_edges; i++) {
    feature_edges.row(i) = uE.row(result[i]);
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
