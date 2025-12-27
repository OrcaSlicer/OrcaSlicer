// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PLANARIZE_QUAD_MESH_H
#define IGL_PLANARIZE_QUAD_MESH_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  // Planarizes a given quad mesh using the algorithm described in the paper
  // "Shape-Up: Shaping Discrete Geometry with Projections" by S. Bouaziz,
  // M. Deuss, Y. Schwartzburg, T. Weise, M. Pauly, Computer Graphics Forum,
  // Volume 31, Issue 5, August 2012, p. 1657-1667
  // (http://dl.acm.org/citation.cfm?id=2346802).
  // The algorithm iterates between projecting each quad to its closest planar
  // counterpart and stitching those quads together via a least squares
  // optimization. It stops whenever all quads' non-planarity is less than a
  // given threshold (suggested value: 0.01), or a maximum number of iterations
  // is reached.

  
  // Inputs:
  //   Vin        #V by 3 eigen Matrix of mesh vertex 3D positions
  //   F          #F by 4 eigen Matrix of face (quad) indices
  //   maxIter    maximum numbers of iterations
  //   threshold  minimum allowed threshold for non-planarity
  // Output:
  //   Vout       #V by 3 eigen Matrix of planar mesh vertex 3D positions
  //
  
  template <typename DerivedV, typename DerivedF>
  IGL_INLINE void planarize_quad_mesh(const Eigen::PlainObjectBase<DerivedV> &Vin,
                                      const Eigen::PlainObjectBase<DerivedF> &F,
                                      const int maxIter,
                                      const double &threshold,
                                      Eigen::PlainObjectBase<DerivedV> &Vout);
}
#ifndef IGL_STATIC_LIBRARY
#  include "planarize_quad_mesh.cpp"
#endif

#endif
