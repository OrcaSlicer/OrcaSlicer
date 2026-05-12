// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_READ_TRIANGLE_MESH_H
#define IGL_COPYLEFT_CGAL_READ_TRIANGLE_MESH_H
#include "../../igl_inline.h"

#include <Eigen/Core>
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Simple wrapper, reads floating point precision but assigns to
      /// DerivedV::Scalar which may be a CGAL type
      ///
      /// @param[in] str  path to file
      /// @param[out] V  eigen double matrix #V by 3
      /// @param[out] F  eigen int matrix #F by 3
      /// @return true iff success
      ///
      /// \see igl::read_triangle_mesh
      template <typename DerivedV, typename DerivedF>
      IGL_INLINE bool read_triangle_mesh(
        const std::string str,
        Eigen::PlainObjectBase<DerivedV>& V,
        Eigen::PlainObjectBase<DerivedF>& F);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "read_triangle_mesh.cpp"
#endif

#endif

