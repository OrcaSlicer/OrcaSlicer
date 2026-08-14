// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2019 Hanxiao Shen <hanxiao@cims.nyu.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_CUT_MESH_H
#define IGL_CUT_MESH_H
#include "igl_inline.h"

#include <Eigen/Core>

namespace igl
{
  /// Given a mesh and a list of edges that are to be cut, the function
  /// generates a new disk-topology mesh that has the cuts at its boundary.
  ///
  ///
  /// \note Assumes mesh is edge-manifold.
  ///
  /// @param[in,out] V  #V by 3 list of the vertex positions
  /// @param[in,out] F  #F by 3 list of the faces
  /// @param[in] cuts  #F by 3 list of boolean flags, so that cuts(i,j)
  ///   indicates that the edge from F(i,j) to F(i,j+1%3) should be cut. A true
  ///   value on either incident face will result in a cut.
  /// @param[out]  I   #V by 1 list of the map between Vn to original V index.
  ///
  /// \note `cuts` assumes the ordering convention from the array-based
  /// triangle_triangle_adjacency which is DIFFERENT from
  /// cotmatrix_entries,edge_lengths/etc.
  ///
  /// \bug `V.conservativeResize(…)` is called. So, although this appears to
  /// edit in place it's likely O(N) even if the number of cuts is O(1); the way
  /// the cuts are specified, just reading them is already O(N)
  template <typename DerivedV, typename DerivedF, typename DerivedC, typename DerivedI>
  IGL_INLINE void cut_mesh(
    Eigen::PlainObjectBase<DerivedV>& V,
    Eigen::PlainObjectBase<DerivedF>& F,
    const Eigen::MatrixBase<DerivedC>& cuts,
    Eigen::PlainObjectBase<DerivedI>& I
  );
  /// Given a mesh and a list of edges that are to be cut, the function
  /// generates a new disk-topology mesh that has the cuts at its boundary.
  ///
  ///
  /// \note Assumes mesh is edge-manifold.
  ///
  /// @param[in,out] FF   #F by #3 adjacent matrix, the element i,j is the id of the triangle
  ///        adjacent to the j edge of triangle i
  /// @param[in,out] FFi  #F by #3 adjacent matrix, the element i,j is the id of edge of the
  ///        triangle TT(i,j) that is adjacent with triangle i
  ///
  /// \see triangle_triangle_adjacency
  template <typename DerivedV, typename DerivedF, typename DerivedFF, typename DerivedFFi, typename DerivedC, typename DerivedI>
  IGL_INLINE void cut_mesh(
    Eigen::PlainObjectBase<DerivedV>& V,
    Eigen::PlainObjectBase<DerivedF>& F,
    Eigen::MatrixBase<DerivedFF>& FF,
    Eigen::MatrixBase<DerivedFFi>& FFi,
    const Eigen::MatrixBase<DerivedC>& C,
    Eigen::PlainObjectBase<DerivedI>& I
  );

  /// Given a mesh and a list of edges that are to be cut, the function
  /// generates a new disk-topology mesh that has the cuts at its boundary.
  ///
  ///
  /// \note Assumes mesh is edge-manifold.
  /// @param[out]  Vn  #V by 3 list of the vertex positions of the cut mesh. This matrix
  ///     will be similar to the original vertices except some rows will be
  ///     duplicated.
  /// @param[out]  Fn  #F by 3 list of the faces of the cut mesh(must be triangles). This
  ///     matrix will be similar to the original face matrix except some indices
  ///     will be redirected to point to the newly duplicated vertices.
  template <typename DerivedV, typename DerivedF, typename DerivedC>
  IGL_INLINE void cut_mesh(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    const Eigen::MatrixBase<DerivedC>& cuts,
    Eigen::PlainObjectBase<DerivedV>& Vn,
    Eigen::PlainObjectBase<DerivedF>& Fn
  );

  /// Given a mesh and a list of edges that are to be cut, the function
  /// generates a new disk-topology mesh that has the cuts at its boundary.
  ///
  ///
  /// \note Assumes mesh is edge-manifold.
  /// @param[in,out] V  #V by 3 list of the vertex positions
  /// @param[in,out] F  #F by 3 list of the faces
  /// @param[in] cuts  #F by 3 list of boolean flags, indicating the edges that need to
  ///     be cut (has 1 at the face edges that are to be cut, 0 otherwise)
  /// @param[out]  Vn  #V by 3 list of the vertex positions of the cut mesh. This matrix
  ///     will be similar to the original vertices except some rows will be
  ///     duplicated.
  /// @param[out]  Fn  #F by 3 list of the faces of the cut mesh(must be triangles). This
  ///     matrix will be similar to the original face matrix except some indices
  ///     will be redirected to point to the newly duplicated vertices.
  /// @param[out]  I   #V by 1 list of the map between Vn to original V index.
  template <
    typename DerivedV, 
    typename DerivedF, 
    typename DerivedC, 
    typename DerivedVn,
    typename DerivedFn,
    typename DerivedI>
  IGL_INLINE void cut_mesh(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    const Eigen::MatrixBase<DerivedC>& cuts,
    Eigen::PlainObjectBase<DerivedVn>& Vn,
    Eigen::PlainObjectBase<DerivedFn>& Fn,
    Eigen::PlainObjectBase<DerivedI>& I);


  
}


#ifndef IGL_STATIC_LIBRARY
#include "cut_mesh.cpp"
#endif


#endif
