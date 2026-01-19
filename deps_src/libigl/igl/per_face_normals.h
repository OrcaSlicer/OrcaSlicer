// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PER_FACE_NORMALS_H
#define IGL_PER_FACE_NORMALS_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  // Compute face normals via vertex position list, face list
  // Inputs:
  //   V  #V by 3 eigen Matrix of mesh vertex 3D positions
  //   F  #F by 3 eigen Matrix of face (triangle) indices
  //   Z  3 vector normal given to faces with degenerate normal.
  // Output:
  //   N  #F by 3 eigen Matrix of mesh face (triangle) 3D normals
  //
  // Example:
  //   // Give degenerate faces (1/3,1/3,1/3)^0.5
  //   per_face_normals(V,F,Vector3d(1,1,1).normalized(),N);
  template <typename DerivedV, typename DerivedF, typename DerivedZ, typename DerivedN>
  IGL_INLINE void per_face_normals(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    const Eigen::MatrixBase<DerivedZ> & Z,
    Eigen::PlainObjectBase<DerivedN> & N);
  // Wrapper with Z = (0,0,0). Note that this means that row norms will be zero
  // (i.e. not 1) for degenerate normals.
  template <typename DerivedV, typename DerivedF, typename DerivedN>
  IGL_INLINE void per_face_normals(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedN> & N);
  // Special version where order of face indices is guaranteed not to effect
  // output.
  template <typename DerivedV, typename DerivedF, typename DerivedN>
  IGL_INLINE void per_face_normals_stable(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedN> & N);
}

#ifndef IGL_STATIC_LIBRARY
#  include "per_face_normals.cpp"
#endif

#endif
