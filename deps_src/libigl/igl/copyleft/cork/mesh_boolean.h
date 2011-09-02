// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CORK_MESH_BOOLEAN_H
#define IGL_COPYLEFT_CORK_MESH_BOOLEAN_H
#include "../../MeshBooleanType.h"
#include "../../igl_inline.h"
#include <Eigen/Core>
#include <cork.h> // for consistent uint

namespace igl
{
  namespace copyleft
  {
    namespace cork
    {
      // Compute a boolean operation on two input meshes using the cork library.
      //
      // Inputs:
      //   VA  #VA by 3 list of vertex positions of first mesh
      //   FA  #FA by 3 list of triangle indices into VA
      //   VB  #VB by 3 list of vertex positions of second mesh
      //   FB  #FB by 3 list of triangle indices into VB
      //   type  of boolean operation see MeshBooleanType.h
      // Outputs:
      //   VC  #VC by 3 list of vertex positions of output mesh
      //   FC  #FC by 3 list of triangle indices into VC
      template <
        typename DerivedVA,
        typename DerivedFA,
        typename DerivedVB,
        typename DerivedFB,
        typename DerivedVC,
        typename DerivedFC>
      IGL_INLINE void mesh_boolean(
        const Eigen::PlainObjectBase<DerivedVA > & VA,
        const Eigen::PlainObjectBase<DerivedFA > & FA,
        const Eigen::PlainObjectBase<DerivedVB > & VB,
        const Eigen::PlainObjectBase<DerivedFB > & FB,
        const MeshBooleanType & type,
        Eigen::PlainObjectBase<DerivedVC > & VC,
        Eigen::PlainObjectBase<DerivedFC > & FC);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "mesh_boolean.cpp"
#endif

#endif
