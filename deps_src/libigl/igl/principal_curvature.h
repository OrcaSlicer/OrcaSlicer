// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PRINCIPAL_CURVATURE_H
#define IGL_PRINCIPAL_CURVATURE_H


#include <Eigen/Geometry>
#include <Eigen/Dense>

#include <vector>

#include "igl_inline.h"



namespace igl
{
  /// Compute the principal curvature directions and magnitude of the given triangle mesh
  ///   DerivedV derived from vertex positions matrix type: i.e. MatrixXd
  ///   DerivedF derived from face indices matrix type: i.e. MatrixXi
  /// @param[in] V       eigen matrix #V by 3
  /// @param[in] F       #F by 3 list of mesh faces (must be triangles)
  /// @param[out] PD1 #V by 3 maximal curvature direction for each vertex.
  /// @param[out] PD2 #V by 3 minimal curvature direction for each vertex.
  /// @param[out] PV1 #V by 1 maximal curvature value for each vertex.
  /// @param[out] PV2 #V by 1 minimal curvature value for each vertex.
  /// @param[in] radius  controls the size of the neighbourhood used, 1 = average edge length
  /// @param[in] useKring  use Kring neighbourhood instead of ball neighbourhood
  /// @return vector of indices of bad vertices if any.
  ///
  /// This function has been developed by: Nikolas De Giorgis, Luigi Rocca and Enrico Puppo.
  /// The algorithm is based on:
  /// Efficient Multi-scale Curvature and Crease Estimation
  /// Daniele Panozzo, Enrico Puppo, Luigi Rocca
  /// GraVisMa, 2010
  ///
  /// \see average_onto_faces, average_onto_vertices
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedPD1,
    typename DerivedPD2,
    typename DerivedPV1,
    typename DerivedPV2>
  IGL_INLINE void principal_curvature(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedPD1>& PD1,
    Eigen::PlainObjectBase<DerivedPD2>& PD2,
    Eigen::PlainObjectBase<DerivedPV1>& PV1,
    Eigen::PlainObjectBase<DerivedPV2>& PV2,
    unsigned radius = 5,
    bool useKring = true);
  /// \overload
  /// @param[out] bad_vertices vector of indices of bad vertices if any.
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedPD1,
    typename DerivedPD2,
    typename DerivedPV1,
    typename DerivedPV2,
    typename Index>
  IGL_INLINE void principal_curvature(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedPD1>& PD1,
    Eigen::PlainObjectBase<DerivedPD2>& PD2,
    Eigen::PlainObjectBase<DerivedPV1>& PV1,
    Eigen::PlainObjectBase<DerivedPV2>& PV2,
    std::vector<Index>& bad_vertices,
    unsigned radius = 5,
    bool useKring = true);
}


#ifndef IGL_STATIC_LIBRARY
#include "principal_curvature.cpp"
#endif

#endif
