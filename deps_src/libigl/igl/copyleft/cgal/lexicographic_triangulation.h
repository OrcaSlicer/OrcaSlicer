// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
//                    Qingan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_COPYLEFT_CGAL_LEXICOGRAPHIC_TRIANGULATION_H
#define IGL_COPYLEFT_CGAL_LEXICOGRAPHIC_TRIANGULATION_H
#include "../../igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {

      // Given a set of points in 2D, return a lexicographic triangulation of these
      // points.
      //
      // Inputs:
      //   P  #P by 2 list of vertex positions
      //
      // Outputs:
      //   F  #F by 3 of faces in lexicographic triangulation.
      template<
        typename DerivedP,
        typename DerivedF
        >
      IGL_INLINE void lexicographic_triangulation(
          const Eigen::PlainObjectBase<DerivedP>& P,
          Eigen::PlainObjectBase<DerivedF>& F);
    }
  }
}




#ifndef IGL_STATIC_LIBRARY
#  include "lexicographic_triangulation.cpp"
#endif
#endif
