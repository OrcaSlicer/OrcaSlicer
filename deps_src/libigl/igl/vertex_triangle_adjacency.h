// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_VERTEX_TRIANGLE_ADJACENCY_H
#define IGL_VERTEX_TRIANGLE_ADJACENCY_H
#include "igl_inline.h"

#include <Eigen/Dense>
#include <vector>

namespace igl
{
  // vertex_face_adjacency constructs the vertex-face topology of a given mesh (V,F)
  //
  // Inputs:
  //   //V  #V by 3 list of vertex coordinates
  //   n  number of vertices #V (e.g. `F.maxCoeff()+1` or `V.rows()`)
  //   F  #F by dim list of mesh faces (must be triangles)
  // Outputs:
  //   VF  #V list of lists of incident faces (adjacency list)
  //   VI  #V list of lists of index of incidence within incident faces listed
  //     in VF
  //
  // See also: edges, cotmatrix, diag, vv
  //
  // Known bugs: this should not take V as an input parameter.
  // Known bugs/features: if a facet is combinatorially degenerate then faces
  // will appear multiple times in VF and correspondingly in VFI (j appears
  // twice in F.row(i) then i will appear twice in VF[j])
  template <typename DerivedF, typename VFType, typename VFiType>
  IGL_INLINE void vertex_triangle_adjacency(
    const typename DerivedF::Scalar n,
    const Eigen::MatrixBase<DerivedF>& F,
    std::vector<std::vector<VFType> >& VF,
    std::vector<std::vector<VFiType> >& VFi);
  template <typename DerivedV, typename DerivedF, typename IndexType>
  IGL_INLINE void vertex_triangle_adjacency(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    std::vector<std::vector<IndexType> >& VF,
    std::vector<std::vector<IndexType> >& VFi);
  // Inputs:
  //   F  #F by 3 list of triangle indices into some vertex list V
  //   n  number of vertices, #V (e.g., F.maxCoeff()+1)
  // Outputs:
  //   VF  3*#F list  List of faces indice on each vertex, so that VF(NI(i)+j) =
  //     f, means that face f is the jth face (in no particular order) incident
  //     on vertex i.
  //   NI  #V+1 list  cumulative sum of vertex-triangle degrees with a
  //     preceeding zero. "How many faces" have been seen before visiting this
  //     vertex and its incident faces.
  template <
    typename DerivedF,
    typename DerivedVF,
    typename DerivedNI>
  IGL_INLINE void vertex_triangle_adjacency(
    const Eigen::MatrixBase<DerivedF> & F,
    const int n,
    Eigen::PlainObjectBase<DerivedVF> & VF,
    Eigen::PlainObjectBase<DerivedNI> & NI);
}

#ifndef IGL_STATIC_LIBRARY
#  include "vertex_triangle_adjacency.cpp"
#endif

#endif
