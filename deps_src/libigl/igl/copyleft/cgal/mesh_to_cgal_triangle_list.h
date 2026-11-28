// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_MESH_TO_CGAL_TRIANGLE_LIST_H
#define IGL_COPYLEFT_CGAL_MESH_TO_CGAL_TRIANGLE_LIST_H
#include "../../igl_inline.h"
#include <Eigen/Core>
#include "CGAL_includes.hpp"
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Convert a mesh (V,F) to a list of CGAL triangles
      ///
      /// @2tparam Kernal  CGAL computation and construction kernel (e.g.
      ///     CGAL::Exact_predicates_exact_constructions_kernel)
      /// @param[in] V  #V by 3 list of vertex positions
      /// @param[in] F  #F by 3 list of triangle indices
      /// @param[out] T  #F list of CGAL triangles
      template <
        typename DerivedV,
        typename DerivedF,
        typename Kernel>
      IGL_INLINE void mesh_to_cgal_triangle_list(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedF> & F,
        std::vector<CGAL::Triangle_3<Kernel> > & T);
    }
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "mesh_to_cgal_triangle_list.cpp"
#endif

#endif
