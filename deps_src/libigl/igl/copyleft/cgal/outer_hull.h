// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_OUTER_HULL_H
#define IGL_COPYLEFT_CGAL_OUTER_HULL_H
#include "../../igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Compute the "outer hull" of a piecewise constant winding number induce
      /// triangle mesh (V,F).
      ///
      /// @param[in] V  #V by 3 list of vertex positions
      /// @param[in] F  #F by 3 list of triangle indices into V
      /// @param[out] HV  #HV by 3 list of output vertex positions
      /// @param[out] HF  #HF by 3 list of output triangle indices into HV
      /// @param[out] J  #HF list of indices into F
      /// @param[out] flip  #HF list of whether facet was flipped when added to HF
      ///
      template <
        typename DerivedV,
        typename DerivedF,
        typename DerivedHV,
        typename DerivedHF,
        typename DerivedJ,
        typename Derivedflip>
      IGL_INLINE void outer_hull(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedF> & F,
        Eigen::PlainObjectBase<DerivedHV> & HV,
        Eigen::PlainObjectBase<DerivedHF> & HF,
        Eigen::PlainObjectBase<DerivedJ> & J,
        Eigen::PlainObjectBase<Derivedflip> & flip);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "outer_hull.cpp"
#endif
#endif
