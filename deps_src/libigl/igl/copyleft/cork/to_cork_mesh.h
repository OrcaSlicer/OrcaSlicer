// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CORK_TO_CORK_MESH_H
#define IGL_COPYLEFT_CORK_TO_CORK_MESH_H
#include "../../igl_inline.h"
#include <cork.h>
#include <Eigen/Core>
namespace igl
{
  namespace copyleft
  {
    namespace cork
    {
      // Convert a (V,F) mesh to a cork's triangle mesh representation.
      //
      // Inputs:
      //   V  #V by 3 list of vertex positions
      //   F  #F by 3 list of triangle indices into V
      // Outputs:
      //   mesh  cork representation of mesh
      template <
        typename DerivedV,
        typename DerivedF>
      IGL_INLINE void to_cork_mesh(
        const Eigen::PlainObjectBase<DerivedV > & V,
        const Eigen::PlainObjectBase<DerivedF > & F,
        CorkTriMesh & mesh);
    }
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "to_cork_mesh.cpp"
#endif
#endif
