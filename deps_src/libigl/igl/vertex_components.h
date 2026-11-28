// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COMPONENTS_H
#define IGL_COMPONENTS_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Sparse>
namespace igl
{
  /// Compute connected components of a graph represented by an adjacency
  /// matrix.
  ///
  /// Outputs a component ID per vertex of the graph where connectivity is established by edges.
  ///
  /// @param[in] A  n by n adjacency matrix
  /// @param[out] C  n list of component ids (starting with 0)
  /// @param[out] counts  #components list of counts for each component
  ///
  template <typename DerivedA, typename DerivedC, typename Derivedcounts>
  IGL_INLINE void vertex_components(
    const Eigen::SparseCompressedBase<DerivedA> & A,
    Eigen::PlainObjectBase<DerivedC> & C,
    Eigen::PlainObjectBase<Derivedcounts> & counts);
  /// \overload
  template <typename DerivedA, typename DerivedC>
  IGL_INLINE void vertex_components(
    const Eigen::SparseCompressedBase<DerivedA> & A,
    Eigen::PlainObjectBase<DerivedC> & C);
  /// Compute the connected components for a mesh given its faces.
  /// Returns a component ID per vertex of the mesh where connectivity is established by edges.
  ///
  /// For computing connected components per face see igl::facet_components
  ///
  ///
  /// @param[in] F  n by 3 list of triangle indices
  /// @param[out] C  max(F) list of component ids
  template <typename DerivedF, typename DerivedC>
  IGL_INLINE void vertex_components(
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedC> & C);

}

#ifndef IGL_STATIC_LIBRARY
#  include "vertex_components.cpp"
#endif

#endif

