// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CORK_FROM_CORK_MESH_H
#define IGL_COPYLEFT_CORK_FROM_CORK_MESH_H
#include "../../igl_inline.h"
#include <cork.h>
#include <Eigen/Core>
namespace igl
{
  namespace copyleft
  {
    namespace cork
    {
      // Convert cork's triangle mesh representation to a (V,F) mesh.
      //
      // Inputs:
      //   mesh  cork representation of mesh
      // Outputs:
      //   V  #V by 3 list of vertex positions
      //   F  #F by 3 list of triangle indices into V
      template <
        typename DerivedV,
        typename DerivedF>
      IGL_INLINE void from_cork_mesh(
        const CorkTriMesh & mesh,
        Eigen::PlainObjectBase<DerivedV > & V,
        Eigen::PlainObjectBase<DerivedF > & F);
    }
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "from_cork_mesh.cpp"
#endif
#endif
